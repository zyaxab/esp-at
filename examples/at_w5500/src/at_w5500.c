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

//! \note Not ideal that they're hard coded but will do for now
#define PIN_NUM_MISO   2
#define PIN_NUM_MOSI   7
#define PIN_NUM_SCLK   6
#define PIN_NUM_CS    18
#define PIN_NUM_INT    8
#define PIN_NUM_RST   -1

static esp_eth_handle_t s_eth_handle = NULL;
static esp_netif_t *s_eth_netif = NULL;

static uint8_t mac_address[6] = { 0 };

static void w5500_event_handler(void * arg, esp_event_base_t event_base, int32_t event_id, void * event_data)
{
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *) arg;

    switch(event_id)
    {
        case ETHERNET_EVENT_CONNECTED:
        {
            ESP_AT_LOGI(TAG, "Ethernet link up");
            
            esp_err_t err = esp_netif_dhcpc_start(s_eth_netif);
            if(err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
            {
                ESP_AT_LOGI(TAG, "DHCP already started");
            }
            else if(err != ESP_OK)
            {
                ESP_AT_LOGE(TAG, "Failed to start DHCP: %s", esp_err_to_name(err));
            }

            break;
        }

        case ETHERNET_EVENT_DISCONNECTED:
            ESP_AT_LOGI(TAG, "Ethernet link down");
            break;

        case ETHERNET_EVENT_START:
            ESP_AT_LOGI(TAG, "Ethernet started");
            break;

        case ETHERNET_EVENT_STOP:
        {
            ESP_AT_LOGI(TAG, "Ethernet stopped");

            esp_err_t err;
            err = esp_netif_dhcpc_stop(s_eth_netif);
            if(err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
            {
                ESP_AT_LOGI(TAG, "DHCP already stopped");
            }
            else if(err != ESP_OK)
            {
                ESP_AT_LOGE(TAG, "Failed to stop DHCP");
            }

            break;
        }

        default:
            ESP_AT_LOGI(TAG, "Unhandled event: %ld", event_id);
            break;
    }
}

static void w5500_got_ip_event_handler(void * arg, esp_event_base_t event_base, int32_t event_id, void * event_data)
{
    ip_event_got_ip_t * event = (ip_event_got_ip_t *) event_data;
    ESP_AT_LOGI(TAG, "Got IP address:" IPSTR, IP2STR(&event->ip_info.ip));
}

static void w5500_lost_ip_event_handler(void * arg, esp_event_base_t event_base, int32_t event_id, void * event_data)
{
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
    // mac_address[0] = 0x1A; mac_address[1] = 0x12; mac_address[2] = 0x34;
    mac_address[0] = 0xBC; mac_address[1] = 0xE9; mac_address[2] = 0x2F;
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

static uint8_t at_query_ethernet_ip(uint8_t * cmd_name)
{
    (void) cmd_name;

    esp_netif_ip_info_t ip_info; 
    if(esp_netif_get_ip_info(s_eth_netif, &ip_info) != ESP_OK)
    {
        ESP_AT_LOGE(TAG, "Failed to query for IP address");
        return ESP_AT_RESULT_CODE_ERROR;
    }

    char resp[64];
    int respLen = snprintf(resp, sizeof(resp), "+ETHIP:" IPSTR "\r\n", IP2STR(&ip_info.ip));
    
    if(esp_at_port_write_data((uint8_t *) resp, respLen) != respLen)
    {
        ESP_AT_LOGE(TAG, "Failed to send response");
        return ESP_AT_RESULT_CODE_ERROR;
    }

    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_set_ethernet_ip(uint8_t para_num)
{
    if((para_num < 1) || (para_num > 3))
        return ESP_AT_RESULT_CODE_ERROR;

    // Gather IP information
    esp_netif_ip_info_t ip_info;
    uint8_t * para = NULL;

    if(esp_at_get_para_as_str(0, &para) != ESP_AT_PARA_PARSE_RESULT_OK)
        return ESP_AT_RESULT_CODE_ERROR;
    ip_info.ip.addr = esp_ip4addr_aton((const char *) para);

    if(esp_at_get_para_as_str(1, &para) == ESP_AT_PARA_PARSE_RESULT_OK)
        ip_info.netmask.addr = esp_ip4addr_aton((const char *) para);

    if(esp_at_get_para_as_str(2, &para) == ESP_AT_PARA_PARSE_RESULT_OK)
        ip_info.gw.addr = esp_ip4addr_aton((const char *) para);

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

static uint8_t at_query_dummy(uint8_t * cmd_name)
{
    {
        esp_netif_dhcp_status_t dhcp_status;
        if(esp_netif_dhcpc_get_status(s_eth_netif, &dhcp_status) == ESP_OK)
        {
            ESP_AT_LOGI(TAG, "DHCP status is %d", dhcp_status);
        }
        else
        {
            ESP_AT_LOGE(TAG, "Failed to query DHCP status");
        }
    }

    {
        uint8_t mac_address[6];
        ESP_ERROR_CHECK(esp_eth_ioctl(s_eth_handle, ETH_CMD_G_MAC_ADDR, mac_address));
        ESP_AT_LOGI(TAG, "Ethernet MAC is actually: %02X:%02X:%02X:%02X:%02X:%02X",
            mac_address[0], mac_address[1], mac_address[2],
            mac_address[3], mac_address[4], mac_address[5]);
    }

    {
        esp_netif_t *def = esp_netif_get_default_netif();
        ESP_AT_LOGI(TAG, "Default netif: %p (desc=%s)", def, esp_netif_get_desc(def));
    }

    {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(s_eth_netif, &ip_info) == ESP_OK) {
            if (ip_info.ip.addr != 0) {
                ESP_AT_LOGI(TAG, "Already has IP: " IPSTR, IP2STR(&ip_info.ip));
            } else {
                ESP_AT_LOGI(TAG, "No IP yet");
            }
        }
    }

    esp_netif_t *netif = NULL;
    netif = esp_netif_next_unsafe(netif);  // Get the first netif

    if (netif == NULL) {
        ESP_AT_LOGI(TAG, "No network interfaces registered.");
        return 0;
    }

    while (netif != NULL) {
        const char *desc = esp_netif_get_desc(netif);
        ESP_AT_LOGI(TAG, "Netif: %p, Desc: %s", netif, desc ? desc : "(no desc)");

        // You can also get more info if needed:
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            ESP_AT_LOGI(TAG, "  IP: " IPSTR ", MASK: " IPSTR ", GW: " IPSTR,
                     IP2STR(&ip_info.ip),
                     IP2STR(&ip_info.netmask),
                     IP2STR(&ip_info.gw));
        } else {
            ESP_AT_LOGI(TAG, "  (No IP info yet)");
        }

        netif = esp_netif_next_unsafe(netif);  // Move to the next one
    }

    return 0;
}

static const esp_at_cmd_struct at_w5500_cmd[] =
{
    { "+ETHIP"     , NULL, at_query_ethernet_ip,  at_set_ethernet_ip,   NULL },
    { "+ETHMAC"    , NULL, at_query_ethernet_mac, NULL,                 NULL },
    { "+DUMMY"     , NULL, at_query_dummy,        NULL,                 NULL },
};

bool esp_at_w5500_cmd_register(void)
{
    if(w5500_init() != ESP_OK)
        return false;

    return esp_at_custom_cmd_array_regist(at_w5500_cmd, sizeof(at_w5500_cmd) / sizeof(esp_at_cmd_struct));
}

ESP_AT_CMD_SET_INIT_FN(esp_at_w5500_cmd_register, 2);