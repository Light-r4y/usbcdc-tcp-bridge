#include <stdio.h>
#include <string.h>
#include <atomic>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "nvs_flash.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/err.h"

#include "usb/cdc_acm_host.h"
#include "usb/vcp_ch34x.hpp"
#include "usb/vcp_cp210x.hpp"
#include "usb/vcp_ftdi.hpp"
#include "usb/vcp.hpp"
#include "usb/usb_host.h"

using namespace esp_usb;

#define WIFI_SSID "ESP32_VCP_Gateway"
#define WIFI_PASS "passw0rd"
#define TCP_PORT 29001
#define BUFFER_SIZE 2048
#define MAX_CLIENTS 1

static const char *TAG = "VCP_GATEWAY";
static SemaphoreHandle_t device_disconnected_sem;
static SemaphoreHandle_t usb_mutex;
static SemaphoreHandle_t tcp_mutex;
static int active_client_socket = -1;
static TaskHandle_t tcp_server_task_handle = NULL;

struct UsbDevice
{
    bool is_vcp_device;
    union
    {
        CdcAcmDevice *vcp_device;
        cdc_acm_dev_hdl_t cdc_handle;
    };
    bool need_close;

    UsbDevice() : is_vcp_device(false), need_close(false)
    {
        cdc_handle = nullptr;
    }

    esp_err_t send_data(const uint8_t *data, size_t len)
    {
        if (!need_close)
            return ESP_FAIL;

        esp_err_t result = ESP_FAIL;

        if (is_vcp_device && vcp_device != nullptr)
        {
            result = vcp_device->tx_blocking((uint8_t *)data, len, 500);
        }
        else if (!is_vcp_device && cdc_handle != nullptr)
        {
            result = cdc_acm_host_data_tx_blocking(cdc_handle, (uint8_t *)data, len, 500);
        }

        return result;
    }
};

static UsbDevice usb_device;
static esp_netif_ip_info_t ip_info;
static std::atomic<bool> usb_device_ready(false);

static void initialize_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "Clearing NVS...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");
}

static void wifi_init_ap(void)
{
    initialize_nvs();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {};
    strlcpy((char *)wifi_config.ap.ssid, WIFI_SSID, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(WIFI_SSID);
    wifi_config.ap.channel = 1;
    wifi_config.ap.beacon_interval = 50;

    esp_wifi_set_ps(WIFI_PS_NONE);

    if (strlen(WIFI_PASS) > 0)
    {
        strlcpy((char *)wifi_config.ap.password, WIFI_PASS, sizeof(wifi_config.ap.password));
        wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    }
    else
    {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    wifi_config.ap.max_connection = MAX_CLIENTS;
    wifi_config.ap.beacon_interval = 100;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_get_ip_info(netif, &ip_info);
    ESP_LOGI(TAG, "WiFi AP: %s, IP: " IPSTR, WIFI_SSID, IP2STR(&ip_info.ip));
}

static bool vcp_rx_callback(const uint8_t *data, size_t data_len, void *arg)
{
    if (!usb_device_ready || data_len == 0)
    {
        return true;
    }

    xSemaphoreTake(tcp_mutex, pdMS_TO_TICKS(5));
    if (active_client_socket >= 0)
    {
        int sent = send(active_client_socket, data, data_len, MSG_DONTWAIT);
        if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            ESP_LOGI(TAG, "TCP send error %d, closing socket", errno);
            close(active_client_socket);
            active_client_socket = -1;
        }
        else if (sent > 0)
        {
            ESP_LOGD(TAG, "Sent %d bytes to TCP", sent);
        }
        else
        {
            ESP_LOGI("vcp_rx_callback . send", "TCP send %d, error %d (EAGAIN)", sent, errno);
            send(active_client_socket, data, data_len, MSG_DONTWAIT);
        }
    }
    xSemaphoreGive(tcp_mutex);

    return true;
}

static void vcp_event_callback(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    switch (event->type)
    {
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGI(TAG, "USB device disconnected");
        usb_device_ready = false;
        xSemaphoreGive(device_disconnected_sem);
        break;

    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGI(TAG, "Serial state changed: 0x%04X", event->data.serial_state.val);
        break;

    case CDC_ACM_HOST_NETWORK_CONNECTION:
        ESP_LOGI(TAG, "Network connection: %s",
                 event->data.network_connected ? "connected" : "disconnected");
        break;

    default:
        ESP_LOGD(TAG, "Unhandled event: %d", event->type);
        break;
    }
}

static void usb_host_task(void *arg)
{
    while (1)
    {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
        {
            usb_host_device_free_all();
        }
    }
}

static bool open_usb_device(void)
{
    cdc_acm_host_device_config_t dev_cfg = {
        .connection_timeout_ms = 5000,
        .out_buffer_size = BUFFER_SIZE,
        .in_buffer_size = BUFFER_SIZE,
        .event_cb = vcp_event_callback,
        .data_cb = vcp_rx_callback,
        .user_arg = NULL};

    usb_device.vcp_device = VCP::open(&dev_cfg);
    if (usb_device.vcp_device != nullptr)
    {
        usb_device.is_vcp_device = true;
        usb_device.need_close = true;
        ESP_LOGI(TAG, "USB device opened via VCP driver");
        usb_device_ready = true;
        return true;
    }

    esp_err_t err = cdc_acm_host_open(0x0483, 0x5740, 1, &dev_cfg, &usb_device.cdc_handle);
    if (err == ESP_OK)
    {
        usb_device.is_vcp_device = false;
        usb_device.need_close = true;
        ESP_LOGI(TAG, "STM32 CDC device connected");
        usb_device_ready = true;
        return true;
    }

    err = cdc_acm_host_open_vendor_specific(0, 0, 1, &dev_cfg, &usb_device.cdc_handle);
    if (err == ESP_OK)
    {
        usb_device.is_vcp_device = false;
        usb_device.need_close = true;
        ESP_LOGI(TAG, "Generic CDC-ACM device connected");
        usb_device_ready = true;
        return true;
    }

    return false;
}

static void close_usb_device(void)
{
    usb_device_ready = false;

    if (usb_device.need_close)
    {
        if (usb_device.is_vcp_device && usb_device.vcp_device != nullptr)
        {
            delete usb_device.vcp_device;
            usb_device.vcp_device = nullptr;
        }
        else if (!usb_device.is_vcp_device && usb_device.cdc_handle != nullptr)
        {
            cdc_acm_host_close(usb_device.cdc_handle);
            usb_device.cdc_handle = nullptr;
        }
    }

    usb_device.is_vcp_device = false;
    usb_device.need_close = false;
}

static void setup_usb_baudrate(uint32_t baudrate = 115200)
{
    if (!usb_device.need_close)
        return;

    cdc_acm_line_coding_t line_coding = {
        .dwDTERate = baudrate,
        .bCharFormat = 0,
        .bParityType = 0,
        .bDataBits = 8,
    };

    if (usb_device.is_vcp_device && usb_device.vcp_device != nullptr)
    {
        usb_device.vcp_device->line_coding_set(&line_coding);
        usb_device.vcp_device->set_control_line_state(true, true);
    }
    else if (!usb_device.is_vcp_device && usb_device.cdc_handle != nullptr)
    {
        cdc_acm_host_line_coding_set(usb_device.cdc_handle, &line_coding);
        cdc_acm_host_set_control_line_state(usb_device.cdc_handle, true, true);
    }

    ESP_LOGI(TAG, "USB baudrate set to %lu", baudrate);
}

static void tcp_server_task(void *arg)
{
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(TCP_PORT);

    int server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (server_socket < 0)
    {
        ESP_LOGE(TAG, "Failed to create socket");
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000;
    setsockopt(server_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    int keep_alive = 1;
    int keep_idle = 30;
    int keep_interval = 5;
    int keep_count = 3;

    setsockopt(server_socket, SOL_SOCKET, SO_KEEPALIVE, &keep_alive, sizeof(keep_alive));
    setsockopt(server_socket, IPPROTO_TCP, TCP_KEEPIDLE, &keep_idle, sizeof(keep_idle));
    setsockopt(server_socket, IPPROTO_TCP, TCP_KEEPINTVL, &keep_interval, sizeof(keep_interval));
    setsockopt(server_socket, IPPROTO_TCP, TCP_KEEPCNT, &keep_count, sizeof(keep_count));

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        ESP_LOGE(TAG, "Failed to bind socket");
        close(server_socket);
        vTaskDelete(NULL);
        return;
    }

    if (listen(server_socket, MAX_CLIENTS) < 0)
    {
        ESP_LOGE(TAG, "Failed to listen on socket");
        close(server_socket);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TCP server listening on port %d", TCP_PORT);

    uint8_t buffer[BUFFER_SIZE];

    while (1)
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        ESP_LOGI(TAG, "Waiting for TCP connection...");
        int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
        if (client_socket < 0)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        char client_ip[16];
        inet_ntoa_r(client_addr.sin_addr, client_ip, sizeof(client_ip));
        ESP_LOGI(TAG, "New TCP client: %s:%d", client_ip, ntohs(client_addr.sin_port));

        timeout.tv_sec = 0;
        timeout.tv_usec = 1000;
        setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        int flag = 1;
        setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));

        xSemaphoreTake(tcp_mutex, portMAX_DELAY);
        if (active_client_socket >= 0)
        {
            ESP_LOGI(TAG, "Closing previous connection");
            shutdown(active_client_socket, SHUT_RDWR);
            close(active_client_socket);
        }
        active_client_socket = client_socket;
        xSemaphoreGive(tcp_mutex);

        bool client_connected = true;
        int usb_consecutive_errors = 0;

        while (client_connected)
        {
            int len = recv(client_socket, buffer, sizeof(buffer), MSG_DONTWAIT);

            if (len > 0)
            {
                ESP_LOGD(TAG, "Received %d bytes from TCP", len);

                if (usb_device_ready)
                {
                    xSemaphoreTake(usb_mutex, portMAX_DELAY);
                    esp_err_t result = usb_device.send_data(buffer, len);
                    // ESP_LOGI("usb_device.send_data", "Send %d len", len );
                    xSemaphoreGive(usb_mutex);

                    if (result == ESP_OK)
                    {
                        // ESP_LOGI(TAG, "Sent %d bytes to USB", len);
                        usb_consecutive_errors = 0;
                    }
                    else
                    {
                        ESP_LOGW(TAG, "Failed to send to USB: 0x%x", result);

                        usb_consecutive_errors++;
                        if (usb_consecutive_errors >= 10) 
                        {
                            ESP_LOGE(TAG, "Too many USB send errors, marking device as not ready");
                            usb_device_ready = false; 
                            client_connected = false;
                        }

                        if (result == ESP_FAIL || result == ESP_ERR_NOT_SUPPORTED)
                        {
                            client_connected = false;
                        }
                    }
                }
                else
                {
                    ESP_LOGW(TAG, "USB device not ready, ignoring data");
                    client_connected = false;
                }
            }
            else if (len == 0)
            {
                ESP_LOGI(TAG, "TCP client disconnected gracefully");
                client_connected = false;
            }
            else
            {
                int err = errno;
                if (err == EAGAIN || err == EWOULDBLOCK)
                {
                    continue;
                }
                else
                {
                    ESP_LOGW(TAG, "TCP recv error: %d", err);
                    client_connected = false;
                }
            }
        }

        xSemaphoreTake(tcp_mutex, portMAX_DELAY);
        if (active_client_socket == client_socket)
        {
            shutdown(client_socket, SHUT_RDWR);
            close(client_socket);
            active_client_socket = -1;
        }
        xSemaphoreGive(tcp_mutex);

        ESP_LOGI(TAG, "Client disconnected");
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    close(server_socket);
    vTaskDelete(NULL);
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Starting VCP to TCP Gateway");

    device_disconnected_sem = xSemaphoreCreateBinary();
    usb_mutex = xSemaphoreCreateMutex();
    tcp_mutex = xSemaphoreCreateMutex();

    if (!device_disconnected_sem || !usb_mutex || !tcp_mutex)
    {
        ESP_LOGE(TAG, "Failed to create semaphores");
        return;
    }

    wifi_init_ap();

    usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .enum_filter_cb = NULL,
        .fifo_settings_custom = {
            .nptx_fifo_lines = 0,
            .ptx_fifo_lines = 0,
            .rx_fifo_lines = 0},
        .peripheral_map = 0};

    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    xTaskCreate(usb_host_task, "usb_host", 4096, NULL, 9, NULL);

    ESP_ERROR_CHECK(cdc_acm_host_install(NULL));

    VCP::register_driver<FT23x>();
    VCP::register_driver<CP210x>();
    VCP::register_driver<CH34x>();

    xTaskCreate(tcp_server_task, "tcp_server", 8192, NULL, 8, &tcp_server_task_handle);

    ESP_LOGI(TAG, "VCP Gateway ready! Connect to:");
    ESP_LOGI(TAG, "WiFi: %s", WIFI_SSID);
    ESP_LOGI(TAG, "TCP: " IPSTR ":%d", IP2STR(&ip_info.ip), TCP_PORT);

    ESP_LOGI(TAG, "Waiting for USB device...");

    while (1)
    {

        if (open_usb_device())
        {
            ESP_LOGI(TAG, "USB device connected");
            setup_usb_baudrate(115200);

            xSemaphoreTake(device_disconnected_sem, portMAX_DELAY);

            ESP_LOGI(TAG, "USB device disconnected");
            close_usb_device();

            xSemaphoreTake(device_disconnected_sem, 0);

            vTaskDelay(pdMS_TO_TICKS(2));
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    vSemaphoreDelete(device_disconnected_sem);
    vSemaphoreDelete(usb_mutex);
    vSemaphoreDelete(tcp_mutex);
}