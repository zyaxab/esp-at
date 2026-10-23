#include "at_socket.h"

#include "esp_at_cmd_register.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AT_SOCK_MAX_LINKS           (5)
#define AT_SOCK_MAX_LISTENERS       (2)
#define AT_SOCK_RX_BUFFER_SIZE      (4096)
#define AT_SOCK_RECV_MAX            (AT_SOCK_RX_BUFFER_SIZE)
#define AT_SOCK_UDP_DATAGRAM_MAX    (1472)
#define AT_SOCK_SEND_CHUNK_SIZE     (1024)
#define AT_SOCK_HEADER_MAX          (48)
#define AT_SOCK_LISTEN_BACKLOG      (2)

#define AT_SOCK_CONNECT_TIMEOUT_MS  (8000)
#define AT_SOCK_SEND_TIMEOUT_MS     (5000)
#define AT_SOCK_DATA_TIMEOUT_MS     (5000)
#define AT_SOCK_POLL_INTERVAL_MS    (20)

#define AT_SOCK_TASK_STACK_SIZE     (4096)
#define AT_SOCK_TASK_PRIORITY       (5)

typedef enum
{
    LINK_FREE = 0,
    LINK_CONNECTING,    //!< Reserved by +SOCKOPEN while it connects
    LINK_OPEN,
    LINK_CLOSED,        //!< Closed by the remote or errored, buffered data can still be read
} at_sock_link_state_t;

typedef struct
{
    at_sock_link_state_t state;
    bool udp;
    int fd;

    //! Received data, always starting at index 0
    uint8_t * rx;
    size_t rx_count;
} at_sock_link_t;

typedef struct
{
    bool active;
    int fd;
    uint16_t port;
} at_sock_listener_t;

static const char * TAG = "at_socket";

//! \note Guards the link & listener tables. It is also held while writing socket events & +SOCKRECV
//! responses, so the host sees them in the same order as the buffer state changed.
static SemaphoreHandle_t s_lock;
static at_sock_link_t s_links[AT_SOCK_MAX_LINKS];
static at_sock_listener_t s_listeners[AT_SOCK_MAX_LISTENERS];

//! Given when the host writes data during the +SOCKSEND data phase
static SemaphoreHandle_t s_data_sema;

static void at_sock_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void at_sock_unlock(void)
{
    xSemaphoreGive(s_lock);
}

//! \note Must be called with the lock held. The event is written in a single call, so it can only
//! ever end up between two writes of another response, never inside one of our payloads.
static void at_sock_emit_event(const char * format, ...)
{
    char event[AT_SOCK_HEADER_MAX];

    va_list args;
    va_start(args, format);
    int length = vsnprintf(event, sizeof(event), format, args);
    va_end(args);

    if((length <= 0) || (length >= sizeof(event)))
        return;

    if(esp_at_port_active_write_data((uint8_t *) event, length) != length)
        ESP_AT_LOGE(TAG, "Failed to send event");
}

//! \note Must be called with the lock held
static void at_sock_link_release(at_sock_link_t * link)
{
    if(link->fd >= 0)
        close(link->fd);

    free(link->rx);

    memset(link, 0x00, sizeof(*link));
    link->fd = -1;
    link->state = LINK_FREE;
}

//! \brief Release a link closed by the remote once the host has read all of its data.
//!
//! \note Must be called with the lock held. Like the stock CIP commands, CLOSED is only reported after
//! the buffer is empty & the link is free again afterwards, so the host doesn't need to close it.
static void at_sock_link_try_finish_close(int id)
{
    at_sock_link_t * link = &s_links[id];
    if((link->state != LINK_CLOSED) || (link->rx_count != 0))
        return;

    at_sock_link_release(link);
    at_sock_emit_event("+SOCK:%d,CLOSED\r\n", id);
}

static size_t at_sock_link_rx_space(const at_sock_link_t * link)
{
    return AT_SOCK_RX_BUFFER_SIZE - link->rx_count;
}

//! \brief Whether the socket task should read from a link. UDP links need room for a whole datagram,
//! otherwise the rest of it would get truncated.
static bool at_sock_link_wants_data(const at_sock_link_t * link)
{
    if(link->state != LINK_OPEN)
        return false;

    return at_sock_link_rx_space(link) >= (link->udp ? AT_SOCK_UDP_DATAGRAM_MAX : 1);
}

static void at_sock_configure_fd(int fd)
{
    struct timeval timeout =
    {
        .tv_sec = AT_SOCK_SEND_TIMEOUT_MS / 1000,
        .tv_usec = (AT_SOCK_SEND_TIMEOUT_MS % 1000) * 1000,
    };

    if(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0)
        ESP_AT_LOGW(TAG, "Failed to set send timeout (errno: %d)", errno);
}

//! \note Must be called with the lock held
static void at_sock_receive(int id)
{
    at_sock_link_t * link = &s_links[id];

    int received = recv(link->fd, link->rx + link->rx_count, at_sock_link_rx_space(link), MSG_DONTWAIT);
    if(received > 0)
    {
        bool was_empty = (link->rx_count == 0);
        link->rx_count += received;

        //! Only notify on empty -> non-empty, the host learns the rest from +SOCKRECV
        if(was_empty)
            at_sock_emit_event("+SOCK:%d,DATA,%u\r\n", id, (unsigned) link->rx_count);

        return;
    }

    if((received < 0) && ((errno == EAGAIN) || (errno == EWOULDBLOCK)))
        return;

    //! \note Braces are needed, the log macros expand to a block
    if(received < 0)
    {
        ESP_AT_LOGW(TAG, "Link %d errored (errno: %d)", id, errno);
    }
    else
    {
        ESP_AT_LOGI(TAG, "Link %d closed by remote", id);
    }

    link->state = LINK_CLOSED;
    at_sock_link_try_finish_close(id);
}

//! \note Must be called with the lock held
static void at_sock_accept(at_sock_listener_t * listener)
{
    int fd = accept(listener->fd, NULL, NULL);
    if(fd < 0)
    {
        ESP_AT_LOGW(TAG, "Accept failed (errno: %d)", errno);
        return;
    }

    int id = -1;
    for(int i = 0; i < AT_SOCK_MAX_LINKS; i++)
    {
        if(s_links[i].state == LINK_FREE)
        {
            id = i;
            break;
        }
    }

    if(id < 0)
    {
        ESP_AT_LOGW(TAG, "No free link for incoming connection on port %u", listener->port);
        close(fd);
        return;
    }

    uint8_t * rx = malloc(AT_SOCK_RX_BUFFER_SIZE);
    if(rx == NULL)
    {
        ESP_AT_LOGE(TAG, "No memory for incoming connection");
        close(fd);
        return;
    }

    at_sock_configure_fd(fd);

    at_sock_link_t * link = &s_links[id];
    link->state = LINK_OPEN;
    link->udp = false;
    link->fd = fd;
    link->rx = rx;
    link->rx_count = 0;

    at_sock_emit_event("+SOCK:%d,ACCEPT,%u\r\n", id, listener->port);
}

static void at_sock_task(void * arg)
{
    for(;;)
    {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        int max_fd = -1;

        //! Snapshot of which fds are being waited on. Commands may close or reuse them while we wait,
        //! so they are checked against the tables again before use.
        int link_fds[AT_SOCK_MAX_LINKS];
        int listener_fds[AT_SOCK_MAX_LISTENERS];

        at_sock_lock();
        for(int i = 0; i < AT_SOCK_MAX_LINKS; i++)
        {
            link_fds[i] = at_sock_link_wants_data(&s_links[i]) ? s_links[i].fd : -1;
            if(link_fds[i] >= 0)
            {
                FD_SET(link_fds[i], &read_fds);
                max_fd = at_max(max_fd, link_fds[i]);
            }
        }

        for(int i = 0; i < AT_SOCK_MAX_LISTENERS; i++)
        {
            listener_fds[i] = s_listeners[i].active ? s_listeners[i].fd : -1;
            if(listener_fds[i] >= 0)
            {
                FD_SET(listener_fds[i], &read_fds);
                max_fd = at_max(max_fd, listener_fds[i]);
            }
        }
        at_sock_unlock();

        if(max_fd < 0)
        {
            vTaskDelay(pdMS_TO_TICKS(AT_SOCK_POLL_INTERVAL_MS));
            continue;
        }

        //! The timeout picks up new links & links whose buffers were drained
        struct timeval timeout =
        {
            .tv_sec = 0,
            .tv_usec = AT_SOCK_POLL_INTERVAL_MS * 1000,
        };

        int ready = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        if(ready < 0)
        {
            //! Most likely an fd that a command closed while we waited
            vTaskDelay(pdMS_TO_TICKS(AT_SOCK_POLL_INTERVAL_MS));
            continue;
        }

        if(ready == 0)
            continue;

        at_sock_lock();
        for(int i = 0; i < AT_SOCK_MAX_LINKS; i++)
        {
            if((link_fds[i] < 0) || !FD_ISSET(link_fds[i], &read_fds))
                continue;

            if((s_links[i].state != LINK_OPEN) || (s_links[i].fd != link_fds[i]))
                continue;

            at_sock_receive(i);
        }

        for(int i = 0; i < AT_SOCK_MAX_LISTENERS; i++)
        {
            if((listener_fds[i] < 0) || !FD_ISSET(listener_fds[i], &read_fds))
                continue;

            if(!s_listeners[i].active || (s_listeners[i].fd != listener_fds[i]))
                continue;

            at_sock_accept(&s_listeners[i]);
        }
        at_sock_unlock();
    }
}

//! \brief Resolve & connect to a remote host
//!
//! \return Connected fd on success, -1 on failure
static int at_sock_connect(const char * host, int32_t port, bool udp)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%" PRId32, port);

    struct addrinfo hints =
    {
        .ai_family = AF_UNSPEC,
        .ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM,
    };

    struct addrinfo * address = NULL;
    int err = getaddrinfo(host, port_str, &hints, &address);
    if((err != 0) || (address == NULL))
    {
        ESP_AT_LOGE(TAG, "Failed to resolve \"%s\" (err: %d)", host, err);
        return -1;
    }

    int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if(fd < 0)
    {
        ESP_AT_LOGE(TAG, "Failed to create socket (errno: %d)", errno);
        goto exit;
    }

    //! Connect without blocking so we can apply our own timeout
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    if(connect(fd, address->ai_addr, address->ai_addrlen) != 0)
    {
        if(errno != EINPROGRESS)
        {
            ESP_AT_LOGE(TAG, "Failed to connect (errno: %d)", errno);
            goto fail;
        }

        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(fd, &write_fds);

        struct timeval timeout =
        {
            .tv_sec = AT_SOCK_CONNECT_TIMEOUT_MS / 1000,
            .tv_usec = (AT_SOCK_CONNECT_TIMEOUT_MS % 1000) * 1000,
        };

        if(select(fd + 1, NULL, &write_fds, NULL, &timeout) <= 0)
        {
            ESP_AT_LOGE(TAG, "Connection timed out");
            goto fail;
        }

        int sock_err = 0;
        socklen_t sock_err_len = sizeof(sock_err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &sock_err_len);
        if(sock_err != 0)
        {
            ESP_AT_LOGE(TAG, "Failed to connect (errno: %d)", sock_err);
            goto fail;
        }
    }

    fcntl(fd, F_SETFL, flags);
    at_sock_configure_fd(fd);

    freeaddrinfo(address);
    return fd;

fail:
    close(fd);
    fd = -1;

exit:
    freeaddrinfo(address);
    return fd;
}

static bool at_sock_send_all(int fd, const uint8_t * data, size_t length)
{
    while(length > 0)
    {
        int sent = send(fd, data, length, 0);
        if(sent < 0)
        {
            ESP_AT_LOGE(TAG, "Send failed (errno: %d)", errno);
            return false;
        }

        data += sent;
        length -= sent;
    }

    return true;
}

static bool at_sock_get_link_id(int32_t para_index, int32_t * id)
{
    if(esp_at_get_para_as_digit(para_index, id) != ESP_AT_PARA_PARSE_RESULT_OK)
        return false;

    return (*id >= 0) && (*id < AT_SOCK_MAX_LINKS);
}

static uint8_t at_setup_cmd_sockopen(uint8_t para_num)
{
    if(para_num != 4)
        return ESP_AT_RESULT_CODE_ERROR;

    int32_t id, port;
    uint8_t * type;
    uint8_t * host;
    if(!at_sock_get_link_id(0, &id))
        return ESP_AT_RESULT_CODE_ERROR;

    if(esp_at_get_para_as_str(1, &type) != ESP_AT_PARA_PARSE_RESULT_OK)
        return ESP_AT_RESULT_CODE_ERROR;

    if(esp_at_get_para_as_str(2, &host) != ESP_AT_PARA_PARSE_RESULT_OK)
        return ESP_AT_RESULT_CODE_ERROR;

    if(esp_at_get_para_as_digit(3, &port) != ESP_AT_PARA_PARSE_RESULT_OK)
        return ESP_AT_RESULT_CODE_ERROR;

    if(at_str_is_null(host) || (port <= 0) || (port > UINT16_MAX))
        return ESP_AT_RESULT_CODE_ERROR;

    bool udp;
    if(strcmp((char *) type, "TCP") == 0)
        udp = false;
    else if(strcmp((char *) type, "UDP") == 0)
        udp = true;
    else
        return ESP_AT_RESULT_CODE_ERROR;

    //! Reserve the link so an incoming connection can't take it while we connect
    at_sock_lock();
    bool in_use = (s_links[id].state != LINK_FREE);
    if(!in_use)
        s_links[id].state = LINK_CONNECTING;
    at_sock_unlock();

    if(in_use)
    {
        ESP_AT_LOGE(TAG, "Link %" PRId32 " is already in use", id);
        return ESP_AT_RESULT_CODE_ERROR;
    }

    ESP_AT_LOGI(TAG, "Opening link %" PRId32 " to %s \"%s\":%" PRId32, id, type, host, port);

    uint8_t * rx = malloc(AT_SOCK_RX_BUFFER_SIZE);
    int fd = (rx != NULL) ? at_sock_connect((char *) host, port, udp) : -1;

    at_sock_lock();
    at_sock_link_t * link = &s_links[id];
    if(fd < 0)
    {
        free(rx);
        link->state = LINK_FREE;
    }
    else
    {
        link->state = LINK_OPEN;
        link->udp = udp;
        link->fd = fd;
        link->rx = rx;
        link->rx_count = 0;
    }
    at_sock_unlock();

    return (fd < 0) ? ESP_AT_RESULT_CODE_ERROR : ESP_AT_RESULT_CODE_OK;
}

static void at_sock_wait_data_cb(void)
{
    xSemaphoreGive(s_data_sema);
}

static uint8_t at_setup_cmd_socksend(uint8_t para_num)
{
    if(para_num != 2)
        return ESP_AT_RESULT_CODE_ERROR;

    int32_t id, length;
    if(!at_sock_get_link_id(0, &id))
        return ESP_AT_RESULT_CODE_ERROR;

    if(esp_at_get_para_as_digit(1, &length) != ESP_AT_PARA_PARSE_RESULT_OK)
        return ESP_AT_RESULT_CODE_ERROR;

    if(length <= 0)
        return ESP_AT_RESULT_CODE_ERROR;

    //! \note Only commands close fds, & commands run one at a time, so the fd stays valid for the
    //! whole send even if the remote closes in the meantime
    at_sock_lock();
    bool open = (s_links[id].state == LINK_OPEN);
    int fd = s_links[id].fd;
    at_sock_unlock();

    if(!open)
        return ESP_AT_RESULT_CODE_ERROR;

    uint8_t * chunk = malloc(AT_SOCK_SEND_CHUNK_SIZE);
    if(chunk == NULL)
        return ESP_AT_RESULT_CODE_ERROR;

    xSemaphoreTake(s_data_sema, 0);
    esp_at_port_enter_specific(at_sock_wait_data_cb);
    esp_at_response_result(ESP_AT_RESULT_CODE_OK_AND_INPUT_PROMPT);

    int32_t received = 0;
    bool failed = false;
    while(received < length)
    {
        int32_t available = esp_at_port_get_data_length();
        if(available <= 0)
        {
            if(xSemaphoreTake(s_data_sema, pdMS_TO_TICKS(AT_SOCK_DATA_TIMEOUT_MS)) != pdTRUE)
            {
                ESP_AT_LOGE(TAG, "Timed out waiting for data, got %" PRId32 " / %" PRId32 " bytes", received, length);
                failed = true;
                break;
            }

            continue;
        }

        int32_t to_read = at_min(at_min(available, length - received), AT_SOCK_SEND_CHUNK_SIZE);
        int32_t read = esp_at_port_read_data(chunk, to_read);
        if(read <= 0)
            continue;

        received += read;

        //! Keep draining the payload after a failure so the host stays in sync
        if(!failed && !at_sock_send_all(fd, chunk, read))
            failed = true;
    }

    esp_at_port_exit_specific();
    free(chunk);

    //! The host may have sent the next command right after the payload
    int32_t pending = esp_at_port_get_data_length();
    if(pending > 0)
        esp_at_port_recv_data_notify(pending, portMAX_DELAY);

    return failed ? ESP_AT_RESULT_CODE_SEND_FAIL : ESP_AT_RESULT_CODE_SEND_OK;
}

static uint8_t at_setup_cmd_sockrecv(uint8_t para_num)
{
    if(para_num != 2)
        return ESP_AT_RESULT_CODE_ERROR;

    int32_t id, max_length;
    if(!at_sock_get_link_id(0, &id))
        return ESP_AT_RESULT_CODE_ERROR;

    if(esp_at_get_para_as_digit(1, &max_length) != ESP_AT_PARA_PARSE_RESULT_OK)
        return ESP_AT_RESULT_CODE_ERROR;

    if(max_length < 0)
        return ESP_AT_RESULT_CODE_ERROR;

    max_length = at_min(max_length, AT_SOCK_RECV_MAX);

    uint8_t * response = malloc(AT_SOCK_HEADER_MAX + max_length + 2);
    if(response == NULL)
        return ESP_AT_RESULT_CODE_ERROR;

    uint8_t result = ESP_AT_RESULT_CODE_OK;

    at_sock_lock();
    at_sock_link_t * link = &s_links[id];
    if((link->state != LINK_OPEN) && (link->state != LINK_CLOSED))
    {
        result = ESP_AT_RESULT_CODE_ERROR;
        goto unlock;
    }

    size_t length = at_min((size_t) max_length, link->rx_count);
    size_t remaining = link->rx_count - length;

    int header_length = snprintf((char *) response, AT_SOCK_HEADER_MAX, "+SOCKRECV:%" PRId32 ",%u,%u,", id, (unsigned) length, (unsigned) remaining);
    memcpy(response + header_length, link->rx, length);
    memcpy(response + header_length + length, "\r\n", 2);

    memmove(link->rx, link->rx + length, remaining);
    link->rx_count = remaining;

    //! Single write, so no event can end up inside the payload
    int32_t total_length = header_length + length + 2;
    if(esp_at_port_write_data(response, total_length) != total_length)
    {
        ESP_AT_LOGE(TAG, "Failed to send response");
        result = ESP_AT_RESULT_CODE_ERROR;
    }

    at_sock_link_try_finish_close(id);

unlock:
    at_sock_unlock();
    free(response);

    return result;
}

static uint8_t at_setup_cmd_sockclose(uint8_t para_num)
{
    if(para_num != 1)
        return ESP_AT_RESULT_CODE_ERROR;

    int32_t id;
    if(!at_sock_get_link_id(0, &id))
        return ESP_AT_RESULT_CODE_ERROR;

    //! \note Closing a free link succeeds, since the remote may have closed it just before
    at_sock_lock();
    at_sock_link_t * link = &s_links[id];
    bool connecting = (link->state == LINK_CONNECTING);
    if((link->state == LINK_OPEN) || (link->state == LINK_CLOSED))
        at_sock_link_release(link);
    at_sock_unlock();

    return connecting ? ESP_AT_RESULT_CODE_ERROR : ESP_AT_RESULT_CODE_OK;
}

static int at_sock_listen(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(fd < 0)
    {
        ESP_AT_LOGE(TAG, "Failed to create socket (errno: %d)", errno);
        return -1;
    }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in address =
    {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if(bind(fd, (struct sockaddr *) &address, sizeof(address)) != 0)
    {
        ESP_AT_LOGE(TAG, "Failed to bind port %u (errno: %d)", port, errno);
        close(fd);
        return -1;
    }

    if(listen(fd, AT_SOCK_LISTEN_BACKLOG) != 0)
    {
        ESP_AT_LOGE(TAG, "Failed to listen on port %u (errno: %d)", port, errno);
        close(fd);
        return -1;
    }

    return fd;
}

static uint8_t at_setup_cmd_socklisten(uint8_t para_num)
{
    if((para_num != 1) && (para_num != 2))
        return ESP_AT_RESULT_CODE_ERROR;

    int32_t port;
    int32_t enable = 1;
    if(esp_at_get_para_as_digit(0, &port) != ESP_AT_PARA_PARSE_RESULT_OK)
        return ESP_AT_RESULT_CODE_ERROR;

    if((para_num == 2) && (esp_at_get_para_as_digit(1, &enable) != ESP_AT_PARA_PARSE_RESULT_OK))
        return ESP_AT_RESULT_CODE_ERROR;

    if((port <= 0) || (port > UINT16_MAX) || ((enable != 0) && (enable != 1)))
        return ESP_AT_RESULT_CODE_ERROR;

    at_sock_lock();
    at_sock_listener_t * existing = NULL;
    at_sock_listener_t * free_slot = NULL;
    for(int i = 0; i < AT_SOCK_MAX_LISTENERS; i++)
    {
        if(s_listeners[i].active && (s_listeners[i].port == port))
            existing = &s_listeners[i];
        else if(!s_listeners[i].active && (free_slot == NULL))
            free_slot = &s_listeners[i];
    }

    //! Stopping a listener leaves its accepted links open
    if(!enable)
    {
        if(existing != NULL)
        {
            close(existing->fd);
            existing->fd = -1;
            existing->active = false;
        }
        at_sock_unlock();

        return (existing != NULL) ? ESP_AT_RESULT_CODE_OK : ESP_AT_RESULT_CODE_ERROR;
    }
    at_sock_unlock();

    if((existing != NULL) || (free_slot == NULL))
        return ESP_AT_RESULT_CODE_ERROR;

    //! \note Only commands modify the listener table, so the slot is still free after unlocking
    int fd = at_sock_listen(port);
    if(fd < 0)
        return ESP_AT_RESULT_CODE_ERROR;

    at_sock_lock();
    free_slot->fd = fd;
    free_slot->port = port;
    free_slot->active = true;
    at_sock_unlock();

    return ESP_AT_RESULT_CODE_OK;
}

//! \brief Lets the host detect whether these commands exist, stock firmware responds with ERROR.
//! Responds with the amount of links, e.g. +SOCKOPEN:5
static uint8_t at_test_cmd_sock(uint8_t * cmd_name)
{
    char response[32];
    int length = snprintf(response, sizeof(response), "%s:%d\r\n", cmd_name, AT_SOCK_MAX_LINKS);
    if(esp_at_port_write_data((uint8_t *) response, length) != length)
        return ESP_AT_RESULT_CODE_ERROR;

    return ESP_AT_RESULT_CODE_OK;
}

static const esp_at_cmd_struct at_socket_cmd[] =
{
    { "+SOCKOPEN"   , at_test_cmd_sock, NULL, at_setup_cmd_sockopen     , NULL },
    { "+SOCKSEND"   , at_test_cmd_sock, NULL, at_setup_cmd_socksend     , NULL },
    { "+SOCKRECV"   , at_test_cmd_sock, NULL, at_setup_cmd_sockrecv     , NULL },
    { "+SOCKCLOSE"  , at_test_cmd_sock, NULL, at_setup_cmd_sockclose    , NULL },
    { "+SOCKLISTEN" , at_test_cmd_sock, NULL, at_setup_cmd_socklisten   , NULL },
};

bool esp_at_socket_cmd_register(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_data_sema = xSemaphoreCreateBinary();
    if((s_lock == NULL) || (s_data_sema == NULL))
        return false;

    for(int i = 0; i < AT_SOCK_MAX_LINKS; i++)
        s_links[i].fd = -1;

    for(int i = 0; i < AT_SOCK_MAX_LISTENERS; i++)
        s_listeners[i].fd = -1;

    if(xTaskCreate(at_sock_task, "at_sock", AT_SOCK_TASK_STACK_SIZE, NULL, AT_SOCK_TASK_PRIORITY, NULL) != pdPASS)
        return false;

    return esp_at_custom_cmd_array_regist(at_socket_cmd, sizeof(at_socket_cmd) / sizeof(esp_at_cmd_struct));
}

ESP_AT_CMD_SET_INIT_FN(esp_at_socket_cmd_register, 1);
