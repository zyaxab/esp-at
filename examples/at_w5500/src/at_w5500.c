#include "at_w5500.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_at_cmd_register.h"
#include "esp_eth_mac_spi.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_defaults.h"
#include "esp_netif_types.h"
#include "esp_netif.h"
#include "esp_system.h"

#include <string.h>

static const char TAG[] = "W5500";

//! URCs
static const char IP_GOT[] = "ETHERNET IP GOT\r\n";
static const char IP_LOST[] = "ETHERNET IP LOST\r\n";
static const char LINK_UP[] = "ETHERNET LINK UP\r\n";
static const char LINK_DOWN[] = "ETHERNET LINK DOWN\r\n";

//! \note Not ideal that they're hard coded but will do for now
#define PIN_NUM_MISO   2
#define PIN_NUM_MOSI   7
#define PIN_NUM_SCLK   6
#define PIN_NUM_CS    18
#define PIN_NUM_INT    8
#define PIN_NUM_RST   -1

//! Local storage
static bool initialized = false;
static esp_eth_handle_t s_eth_handle = NULL;
static esp_netif_t *s_eth_netif = NULL;
static uint8_t mac_address[6] = { 0 };

#define ETH_STA_ENABLED 2
#define ETH_STA_DISABLED 0
static int eth_sta_state = ETH_STA_DISABLED; // Must be two to spoof enabled

static esp_err_t send_urc(const char * urc, size_t urc_sizeof)
{
    if(esp_at_port_write_data((uint8_t *) urc, urc_sizeof - 1) != (urc_sizeof - 1))
        return ESP_FAIL;

    return ESP_OK;
}

static void w5500_event_handler(void * arg, esp_event_base_t event_base, int32_t event_id, void * event_data)
{
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *) arg;

    switch(event_id)
    {
        case ETHERNET_EVENT_CONNECTED:
            ESP_AT_LOGI(TAG, "Ethernet link up");
            if(send_urc(LINK_UP, sizeof(LINK_UP)) != ESP_OK)
            {
                ESP_AT_LOGE(TAG, "Failed to send link up URC");
            }

            break;

        case ETHERNET_EVENT_DISCONNECTED:
            eth_sta_state = ETH_STA_DISABLED;
            ESP_AT_LOGI(TAG, "Ethernet link down");
            if(send_urc(LINK_DOWN, sizeof(LINK_DOWN)) != ESP_OK)
            {
                ESP_AT_LOGE(TAG, "Failed to send link down URC");
            }

            break;

        case ETHERNET_EVENT_START:
            ESP_AT_LOGI(TAG, "Ethernet started");
            break;

        case ETHERNET_EVENT_STOP:
        {
            eth_sta_state = ETH_STA_DISABLED;
            ESP_AT_LOGI(TAG, "Ethernet stopped");
            break;
        }

        default:
            ESP_AT_LOGI(TAG, "Unhandled event: %ld", event_id);
            break;
    }
}

static void w5500_got_ip_event_handler(void * arg, esp_event_base_t event_base, int32_t event_id, void * event_data)
{
    eth_sta_state = ETH_STA_ENABLED;

    if(send_urc(IP_GOT, sizeof(IP_GOT)) != ESP_OK)
    {
        ESP_AT_LOGE(TAG, "Failed to send got IP URC");
    }

    ip_event_got_ip_t * event = (ip_event_got_ip_t *) event_data;
    ESP_AT_LOGI(TAG, "Got IP address:" IPSTR, IP2STR(&event->ip_info.ip));

    esp_netif_set_default_netif(s_eth_netif);
}

static void w5500_lost_ip_event_handler(void * arg, esp_event_base_t event_base, int32_t event_id, void * event_data)
{
    eth_sta_state = ETH_STA_DISABLED;

    if(send_urc(IP_LOST, sizeof(IP_LOST)) != ESP_OK)
    {
        ESP_AT_LOGE(TAG, "Failed to send lost IP URC");
    }

    ESP_AT_LOGI(TAG, "Lost IP");
}

esp_err_t w5500_init(void)
{
    // Create default Ethernet netif
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);

    ESP_AT_LOGI(TAG, "Allocated new network interface handle: %p", s_eth_netif);
    assert(s_eth_netif);

    // Initialize SPI
    spi_bus_config_t bus_cfg =
    {
        .miso_io_num = PIN_NUM_MISO,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    ESP_AT_LOGI(TAG, "Initializing SPI bus");
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    // Initialize SPI device
    spi_device_interface_config_t dev_cfg = {
        .command_bits = 16,
        .address_bits = 8,
        .mode = 0,
        .clock_speed_hz = 25 * 1000 * 1000, // 25 MHz
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 20,
    };

    // Create MAC & PHY instances
    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &dev_cfg);
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = PIN_NUM_RST;

    ESP_AT_LOGI(TAG, "Creating ETH instances");
    esp_eth_mac_t * mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    esp_eth_phy_t * phy = esp_eth_phy_new_w5500(&phy_config);
    ESP_AT_LOGI(TAG, "ETH instances created, %p & %p", mac, phy);

    // Ethernet driver config
    ESP_AT_LOGI(TAG, "Installing ETH driver")

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&config, &s_eth_handle));

    // Attach to NETIF
    ESP_AT_LOGI(TAG, "Attaching to netif");
    ESP_ERROR_CHECK(esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(s_eth_handle)));

    // Register event handlers
    ESP_AT_LOGI(TAG, "Setting event handlers");
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &w5500_event_handler, &s_eth_handle));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &w5500_got_ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, &w5500_lost_ip_event_handler, NULL));

    // Set MAC address
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac_address));
    mac_address[0] = 0x02; mac_address[1] = 0x11; mac_address[2] = 0x22;
    ESP_AT_LOGI(TAG, "Ethernet MAC set to: %02X:%02X:%02X:%02X:%02X:%02X",
        mac_address[0], mac_address[1], mac_address[2],
        mac_address[3], mac_address[4], mac_address[5]);

    ESP_ERROR_CHECK(esp_eth_ioctl(s_eth_handle, ETH_CMD_S_MAC_ADDR, mac_address));
    ESP_ERROR_CHECK(esp_netif_set_mac(s_eth_netif, mac_address));

    // Start ethernet
    ESP_ERROR_CHECK(esp_eth_start(s_eth_handle));
    
    ESP_AT_LOGI(TAG, "W5500 Ethernet successfully initialized");

    return ESP_OK;
}

static uint8_t at_exec_ethernet_init(uint8_t * cmd_name)
{
    if(initialized)
        return ESP_AT_RESULT_CODE_ERROR;

    if(w5500_init() != ESP_OK)
        return ESP_AT_RESULT_CODE_ERROR;

    initialized = true;

    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_query_ethernet_ip(uint8_t * cmd_name)
{
    (void) cmd_name;

    if(!initialized)
        return ESP_AT_RESULT_CODE_ERROR;

    esp_netif_ip_info_t ip_info; 
    if(esp_netif_get_ip_info(s_eth_netif, &ip_info) != ESP_OK)
    {
        ESP_AT_LOGE(TAG, "Failed to query for IP address");
        return ESP_AT_RESULT_CODE_ERROR;
    }

    static const char formatter[] =
        "+ETHIP:ip:"       IPSTR "\r\n"
        "+ETHIP:gateway:"  IPSTR "\r\n"
        "+ETHPIP:netmask:" IPSTR "\r\n";

    char resp[128];
    int respLen = snprintf(resp, sizeof(resp), formatter, IP2STR(&ip_info.ip), IP2STR(&ip_info.gw), IP2STR(&ip_info.netmask));
    
    if(esp_at_port_write_data((uint8_t *) resp, respLen) != respLen)
    {
        ESP_AT_LOGE(TAG, "Failed to send response");
        return ESP_AT_RESULT_CODE_ERROR;
    }

    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_set_ethernet_ip(uint8_t para_num)
{
    if((!initialized) || (para_num < 1) || (para_num > 3))
        return ESP_AT_RESULT_CODE_ERROR;

    // Gather IP information
    esp_netif_ip_info_t ip_info;
    uint8_t * para = NULL;

    if(esp_at_get_para_as_str(0, &para) != ESP_AT_PARA_PARSE_RESULT_OK)
        return ESP_AT_RESULT_CODE_ERROR;
        
    ip_info.ip.addr = esp_ip4addr_aton((const char *) para);

    if(esp_at_get_para_as_str(1, &para) == ESP_AT_PARA_PARSE_RESULT_OK)
        ip_info.netmask.addr = esp_ip4addr_aton((const char *) para);
    else
        ip_info.netmask.addr = 0xFFFFFF00;

    if(esp_at_get_para_as_str(2, &para) == ESP_AT_PARA_PARSE_RESULT_OK)
        ip_info.gw.addr = esp_ip4addr_aton((const char *) para);
    else
        ip_info.gw.addr = (ip_info.ip.addr & 0xFFFFFF00) | 0x01;

    if(ip_info.ip.addr == ip_info.gw.addr)
    {
        ESP_AT_LOGE(TAG, "IP address can't be the same as the gateway");
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // Stop DHCP if not already running
    esp_err_t err = esp_netif_dhcpc_stop(s_eth_netif);
    if (err == ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED) {
        ESP_AT_LOGI(TAG, "DHCP client already stopped, no action");
    } else if (err != ESP_OK) {
        ESP_AT_LOGE(TAG, "Failed to stop DHCP client: %s", esp_err_to_name(err));
    }

    // Apply static IP configuration
    err = esp_netif_set_ip_info(s_eth_netif, &ip_info);
    if(err != ESP_OK)
    {
        ESP_AT_LOGE(TAG, "Failed to set new static IP config: %s", esp_err_to_name(err))
        return ESP_AT_RESULT_CODE_ERROR;
    }

    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_query_ethernet_mac(uint8_t * cmd_name)
{
    if(!initialized)
        return ESP_AT_RESULT_CODE_ERROR;

    char resp[64];
    int respLen = snprintf(resp, sizeof(resp), "+ETHMAC:%02X:%02X:%02X:%02X:%02X:%02X\r\n", 
        mac_address[0], mac_address[1], mac_address[2], mac_address[3], mac_address[4], mac_address[5]);

    if(esp_at_port_write_data((uint8_t *) resp, respLen) != respLen)
    {
        ESP_AT_LOGE(TAG, "Failed to send response");
        return ESP_AT_RESULT_CODE_ERROR;
    }

    return ESP_AT_RESULT_CODE_OK;
}

static const esp_at_cmd_struct at_w5500_cmd[] =
{
    { "+ETHINIT"   , NULL, NULL,                  NULL,                 at_exec_ethernet_init },
    { "+ETHIP"     , NULL, at_query_ethernet_ip,  at_set_ethernet_ip,   NULL },
    { "+ETHMAC"    , NULL, at_query_ethernet_mac, NULL,                 NULL },
};

bool esp_at_w5500_cmd_register(void)
{
    return esp_at_custom_cmd_array_regist(at_w5500_cmd, sizeof(at_w5500_cmd) / sizeof(esp_at_cmd_struct));
}

ESP_AT_CMD_SET_INIT_FN(esp_at_w5500_cmd_register, 2);

int at_eth_get_sta_state(void) {
    return eth_sta_state;
}