#include "eth.h"
#include <string.h>
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "web.h"

static const char *TAG = "eth";
#define PHY_RESET_GPIO 51   /* IP101 RESET# on the Waveshare P4 boards */
#define TX_BUF 131072       /* bridge -> clients, see tx_task */
#define TX_MSG 16384        /* largest payload per message = one send() / one WebSocket frame */

static char ip_str[16] = "0.0.0.0";
static volatile int client_fd = -1;
static StreamBufferHandle_t rx_sb;
static MessageBufferHandle_t tx_mb;  /* bridge -> TCP or WebSocket client; eth_tx_enqueue() only copies, tx_task sends */

/* Hands the queued bytes to lwIP (TCP client) or to web.c (WebSocket
 * client). Keeping this off the bridge loop lets the next SDIO read start
 * while the previous frame is still being copied into TCP segments (~0.8 ms
 * per 32 KB). Messages come in pairs: a 1-byte destination tag, then the
 * payload (at most TX_MSG bytes). Without the client the data is dropped. */
static void tx_task(void *arg) {
    static uint8_t buf[TX_MSG];
    uint8_t to = 0;
    for (;;) {
        size_t n = xMessageBufferReceive(tx_mb, buf, sizeof(buf), portMAX_DELAY);
        if (!to) { to = buf[0]; continue; }         /* the tag; the payload follows */
        if (to == ETH_TX_WS) web_ws_send(buf, n);
        else {
            int fd = client_fd;
            const uint8_t *p = buf;
            while (fd >= 0 && n) {
                int s = send(fd, p, n, 0);
                if (s <= 0) break;
                p += s; n -= s;
            }
        }
        to = 0;
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
    ip_event_got_ip_t *e = data;
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&e->ip_info.ip));
    ESP_LOGI(TAG, "IP %s", ip_str);
}

static void tcp_task(void *arg) {
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(ETH_TCP_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    bind(ls, (struct sockaddr *)&a, sizeof(a));
    listen(ls, 1);
    uint8_t buf[2048];
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        ESP_LOGI(TAG, "client connected");
        xStreamBufferReset(rx_sb);
        client_fd = fd;
        for (;;) {
            int n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            xStreamBufferSend(rx_sb, buf, n, portMAX_DELAY);
        }
        client_fd = -1;
        close(fd);
        ESP_LOGI(TAG, "client gone");
    }
}

void eth_start(void) {
    rx_sb = xStreamBufferCreate(16384, 1);
    tx_mb = xMessageBufferCreate(TX_BUF);
    xTaskCreatePinnedToCore(tx_task, "eth_tx", 4096, NULL, 6, NULL, 1);
    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&ncfg);

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();   /* P4: MDC 31, MDIO 52, REF_CLK in on 50 */
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = -1;             /* auto-detect */
    phy_cfg.reset_gpio_num = PHY_RESET_GPIO;
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_cfg);

    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = NULL;
    esp_err_t e = esp_eth_driver_install(&cfg, &eth);
    if (e != ESP_OK) { ESP_LOGE(TAG, "driver install: %s", esp_err_to_name(e)); return; }
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip, NULL));
    ESP_ERROR_CHECK(esp_eth_start(eth));
    xTaskCreatePinnedToCore(tcp_task, "tcp", 4096, NULL, 5, NULL, 1);
}

const char *eth_ip(void) { return ip_str; }
bool eth_client_connected(void) { return client_fd >= 0; }
bool eth_tx_idle(void) { return !tx_mb || xMessageBufferIsEmpty(tx_mb); }

int eth_read(void *buf, size_t n, uint32_t wait_ms) {
    return (int)xStreamBufferReceive(rx_sb, buf, n, pdMS_TO_TICKS(wait_ms));
}

/* Queue `n` bytes for `to` as tag+payload message pairs. Only the bridge
 * loop writes here, so once both messages fit they cannot fail; the wait
 * for space gives up after a second (client too slow: this chunk is lost). */
bool eth_tx_enqueue(uint8_t to, const void *buf, size_t n) {
    if (!tx_mb) return false;
    const uint8_t *p = buf;
    int64_t deadline = esp_timer_get_time() + 1000000;
    while (n) {
        size_t k = n > TX_MSG ? TX_MSG : n;
        size_t need = (1 + sizeof(size_t)) + (k + sizeof(size_t)) + 8;   /* two messages with their length words, margin */
        while (xMessageBufferSpacesAvailable(tx_mb) < need) {
            if (esp_timer_get_time() > deadline) return false;
            vTaskDelay(1);
        }
        xMessageBufferSend(tx_mb, &to, 1, 0);
        xMessageBufferSend(tx_mb, p, k, 0);
        p += k; n -= k;
    }
    return true;
}

bool eth_write(const void *buf, size_t n) {
    if (client_fd < 0) return false;
    return eth_tx_enqueue(ETH_TX_TCP, buf, n);
}
