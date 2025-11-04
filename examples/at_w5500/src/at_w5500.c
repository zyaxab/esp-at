#include "at_w5500.h"

#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_netif.h"
#include "esp_system.h"

static const char TAG[] = "W5500";

//! \note Not ideal that they're hard coded but will do for now
#define PIN_NUM_MISO   2
#define PIN_NUM_MOSI   7
#define PIN_NUM_SCLK   6
#define PIN_NUM_CS    16
#define PIN_NUM_INT    8
#define PIN_NUM_RST   -1

static esp_eth_handle_t s_eth_handle = NULL;
static esp_netif_t *s_eth_netif = NULL;

static void w5500_event_handler(void * arg, esp_event_base_t event_base, int32_t event_id, void * event_data)
{
    uint8_t mac_addr[6];
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *) arg;

    switch(event_id)
    {
        case ETHERNET_EVENT_CONNECTED:
            esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
            ESP_AT_LOGI(TAG, "Ethernet link up");
            ESP_AT_LOGI(TAG, "Ethernet HW address: %02X:%02X:%02X:%02X:%02X:%02X", mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
            break;

        case ETHERNET_EVENT_DISCONNECTED:
            ESP_AT_LOGI(TAG, "Ethernet link down");
            break;

        case ETHERNET_EVENT_START:
            ESP_AT_LOGI(TAG, "Ethernet started");
            break;

        case ETHERNET_EVENT_STOP:
            ESP_AT_LOGI(TAG, "Ethernet stopped");
            break;

        default:
            ESP_AT_LOGI(TAG, "Unhandled event: %ld", event_id);
            break;
    }
}

static void w5500_got_ip_event_handler(void * arg, esp_event_base_t event_base, int32_t event_id, void * event_data)
{
    ip_event_got_ip_t * event = (ip_event_got_ip_t *) event_data;
    ESP_AT_LOGI(TAG, "Got IP address:" IPSTR, IP2STR(&event->ip_onfi.ip));
}

esp_err_t w5500_init(void)
{
    if (!esp_netif_is_netif_list_initialized()) {
        ESP_ERROR_CHECK(esp_netif_init());
    }

    if (esp_event_loop_get_default() == NULL) {
        ESP_ERROR_CHECK(esp_event_loop_create_default());
    }

    // Create default Ethernet netif
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
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
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&devcfg, &SPI2_HOST);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&SPI2_HOST);

    // Ethernet driver config
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&config, &s_eth_handle));

    // Attach to NETIF
    ESP_ERROR_CHECK(esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(s_eth_handle)));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, &s_eth_handle));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    // Start ethernet
    ESP_ERROR_CHECK(esp_eth_start(s_eth_handle));

    ESP_AT_LOGI(TAG, "W5500 Ethernet successfully initialized");

    return ESP_OK;
}