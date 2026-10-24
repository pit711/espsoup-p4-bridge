/* HTTP server (port 80) for the ESPsoup web app plus a WebSocket that carries
 * the C6 byte stream, see web.h.
 *
 *   GET /          -> /index.html
 *   GET /<path>    -> /www/<path>.gz with "Content-Encoding: gzip" if present,
 *                     else /www/<path>, else 404 (chunked, FILE_CHUNK bytes)
 *   GET /ws        -> WebSocket: binary (or text) frames are raw protocol
 *                     bytes in both directions; one client at a time, a new
 *                     one replaces the old
 *
 * The file handler runs in the httpd task (core 1) and reads the LittleFS
 * partition through its memory mapping. Bytes from the WebSocket client go
 * into ws_rx_sb (the bridge loop drains it like the TCP buffer); bytes for
 * the client go through the shared message buffer and sender task of eth.c
 * (tag ETH_TX_WS), which calls web_ws_send() for each message, i.e. one
 * binary frame of at most 16 KB. */
#include "web.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "eth.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "web";
#define WWW_LABEL "www"
#define WWW_BASE "/www"
#define WWW_FALLBACK_OFFSET 0x420000   /* same place as the "www" entry in partitions.csv */
#define WWW_FALLBACK_SIZE   0x600000
#define FILE_CHUNK 8192            /* file -> HTTP chunk size */
#define WS_RX_BUF 16384            /* client -> bridge */
#define WS_RX_PART 4096            /* one read of an incoming frame */
#define ERASE_STEP 0x10000         /* 64 KB block erase while an image is written */

static httpd_handle_t hd;
static volatile int ws_fd = -1;                      /* the one WebSocket client, -1 = none */
static StreamBufferHandle_t ws_rx_sb;
static SemaphoreHandle_t www_lock;                   /* held while a file is sent or the image is replaced */
static volatile bool www_mounted;
static const esp_partition_t *www_part;
static uint8_t file_buf[FILE_CHUNK];                 /* httpd task only */
static uint8_t ws_rx_part[WS_RX_PART];               /* httpd task only */
static struct { uint32_t off, erased, size; } www_up; /* "P4 WWW" progress */

/* ---- static files ------------------------------------------------------- */
static const struct { const char *ext, *type; } types[] = {
    {"html", "text/html; charset=utf-8"},
    {"js", "text/javascript; charset=utf-8"},
    {"mjs", "text/javascript; charset=utf-8"},
    {"css", "text/css; charset=utf-8"},
    {"json", "application/json"},
    {"map", "application/json"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"svg", "image/svg+xml"},
    {"ico", "image/x-icon"},
    {"wasm", "application/wasm"},
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
    {"txt", "text/plain; charset=utf-8"},
    {"md", "text/markdown; charset=utf-8"},
    {"csv", "text/csv; charset=utf-8"},
    {"bin", "application/octet-stream"},
    {"webmanifest", "application/manifest+json"},
};

static const char *content_type(const char *path) {
    const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
    if (dot && (!slash || dot > slash))
        for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
            if (!strcasecmp(dot + 1, types[i].ext)) return types[i].type;
    return "application/octet-stream";
}

static int hexval(char c) { return isdigit((unsigned char)c) ? c - '0' : (tolower((unsigned char)c) - 'a' + 10); }

/* "/a/b?x=1" -> "/www/a/b" (percent-decoded), "/" or "/dir/" get index.html.
 * false for anything that must not reach the file system: "..", control
 * characters, backslashes, or a path longer than `cap`. */
static bool fs_path(const char *uri, char *out, size_t cap) {
    size_t n = strlcpy(out, WWW_BASE, cap);
    for (const char *s = uri; *s && *s != '?' && *s != '#'; s++) {
        char c = *s;
        if (c == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) { c = (char)(hexval(s[1]) * 16 + hexval(s[2])); s += 2; }
        if ((unsigned char)c < 0x20 || c == '\\') return false;
        if (n + 1 >= cap) return false;
        out[n++] = c; out[n] = 0;
    }
    if (strstr(out, "..")) return false;
    if (out[n - 1] == '/') { if (n + sizeof("index.html") > cap) return false; n += strlcpy(out + n, "index.html", cap - n); }
    return true;
}

static esp_err_t reply_503(httpd_req_t *req, const char *msg) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t file_get(httpd_req_t *req) {
    char path[288];
    if (!fs_path(req->uri, path, sizeof(path) - sizeof(".gz"))) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such file");
    if (xSemaphoreTake(www_lock, 0) != pdTRUE) return reply_503(req, "www busy (image update)");
    if (!www_mounted) { xSemaphoreGive(www_lock); return reply_503(req, "no web app image (upload one with \"P4 WWW\")"); }
    const char *type = content_type(path);
    size_t len = strlen(path);
    bool gz = true;
    memcpy(path + len, ".gz", sizeof(".gz"));
    int fd = open(path, O_RDONLY);
    if (fd < 0) { path[len] = 0; gz = false; fd = open(path, O_RDONLY); }
    if (fd < 0) { xSemaphoreGive(www_lock); return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such file"); }
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    if (gz) { httpd_resp_set_hdr(req, "Content-Encoding", "gzip"); httpd_resp_set_hdr(req, "Vary", "Accept-Encoding"); }
    esp_err_t e = ESP_OK;
    for (;;) {
        int n = read(fd, file_buf, sizeof(file_buf));
        if (n < 0) { ESP_LOGW(TAG, "read %s: errno %d", path, errno); e = ESP_FAIL; break; }
        if (n == 0) break;
        e = httpd_resp_send_chunk(req, (const char *)file_buf, n);
        if (e != ESP_OK) break;
    }
    close(fd);
    xSemaphoreGive(www_lock);
    if (e == ESP_OK) e = httpd_resp_send_chunk(req, NULL, 0);   /* end of the chunked body */
    return e;                                                   /* != ESP_OK closes the socket */
}

/* ---- WebSocket ------------------------------------------------------------ */
static bool is_data(httpd_ws_type_t t) { return t == HTTPD_WS_TYPE_BINARY || t == HTTPD_WS_TYPE_TEXT || t == HTTPD_WS_TYPE_CONTINUE; }

/* Make `fd` the one WebSocket client. IDF 6.2 does not call the URI handler
 * for the handshake itself ("do not call the uri->handler" in httpd_uri.c),
 * so this runs on the first data frame of a new socket; the HTTP_GET branch
 * below stays for IDF versions that do call it. */
static void ws_adopt(httpd_req_t *req, int fd) {
    int old = ws_fd;
    ws_fd = -1;                                  /* web_ws_send discards, web_ws_write refuses, until the switch is done */
    if (old >= 0 && old != fd) {
        ESP_LOGI(TAG, "ws client %d replaces %d", fd, old);
        shutdown(old, SHUT_RDWR);                /* frees the sender task if it is stuck in send() on the old socket */
        httpd_sess_trigger_close(req->handle, old);
    }
    for (int i = 0; i < 100 && !eth_tx_idle(); i++) vTaskDelay(1);   /* let queued output for the old client drain (dropped) */
    xStreamBufferReset(ws_rx_sb);                /* fails harmlessly if the bridge loop is blocked in a read */
    ws_fd = fd;
    ESP_LOGI(TAG, "ws client %d connected", fd);
}

static esp_err_t ws_handler(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) { ws_adopt(req, fd); return ESP_OK; }   /* handshake just completed */
    if (fd != ws_fd) ws_adopt(req, fd);          /* first frame of a new client */
    /* A frame: read it in parts; data frames (binary, text, continuation) go
     * to the bridge, PONG (passed by httpd) is consumed and dropped. */
    httpd_ws_frame_t f = {.payload = ws_rx_part};
    size_t have = 0;
    for (;;) {
        esp_err_t e = httpd_ws_recv_frame_part(req, &f, sizeof(ws_rx_part));
        if (e != ESP_OK) { ESP_LOGW(TAG, "ws recv: %s", esp_err_to_name(e)); return e; }
        size_t part = f.len - f.left_len - have;
        have += part;
        if (part && fd == ws_fd && is_data(f.type)) {
            size_t k = xStreamBufferSend(ws_rx_sb, ws_rx_part, part, pdMS_TO_TICKS(2000));
            if (k < part) ESP_LOGW(TAG, "ws rx: bridge loop too slow, %u bytes dropped", (unsigned)(part - k));
        }
        if (!f.left_len) break;
    }
    return ESP_OK;
}

/* One binary frame to the client, called by the sender task in eth.c for
 * each queued message. Without a client (or once it is gone) the data is
 * dropped. The socket is written by the sender task only; httpd itself
 * writes on it just for PONG/CLOSE replies, which browsers do not provoke. */
bool web_ws_send(const void *buf, size_t n) {
    int fd = ws_fd;
    if (fd < 0) return false;
    httpd_ws_frame_t f = {.type = HTTPD_WS_TYPE_BINARY, .payload = (uint8_t *)buf, .len = n};
    if (httpd_ws_send_frame_async(hd, fd, &f) == ESP_OK) {
        httpd_sess_update_lru_counter(hd, fd);   /* keep the stream out of the LRU purge while it is busy */
        return true;
    }
    if (ws_fd == fd) {
        ESP_LOGW(TAG, "ws send failed, client %d dropped", fd);
        ws_fd = -1;
        httpd_sess_trigger_close(hd, fd);
    }
    return false;
}

static esp_err_t on_open(httpd_handle_t h, int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));   /* short replies must not wait for an ACK */
    return ESP_OK;
}

static void on_close(httpd_handle_t h, int fd) {
    if (fd == ws_fd) { ws_fd = -1; ESP_LOGI(TAG, "ws client %d gone", fd); }
    close(fd);   /* with close_fn set, httpd leaves the close to us */
}

/* ---- LittleFS partition --------------------------------------------------- */
static esp_err_t www_mount(void) {
    if (!www_part) return ESP_ERR_NOT_FOUND;
    esp_vfs_littlefs_conf_t c = {.base_path = WWW_BASE, .partition_label = WWW_LABEL, .read_only = 1};
    esp_err_t e = esp_vfs_littlefs_register(&c);
    www_mounted = e == ESP_OK;
    if (e == ESP_OK) {
        size_t total = 0, used = 0;
        esp_littlefs_info(WWW_LABEL, &total, &used);
        ESP_LOGI(TAG, "www mounted, %u of %u KB used", (unsigned)(used / 1024), (unsigned)(total / 1024));
    } else ESP_LOGW(TAG, "www not mounted (%s): no web app until an image is flashed or uploaded with \"P4 WWW\"", esp_err_to_name(e));
    return e;
}

bool web_www_mounted(void) { return www_mounted; }
uint32_t web_www_size(void) { return www_part ? www_part->size : 0; }

esp_err_t web_www_begin(uint32_t size) {
    if (!www_part) return ESP_ERR_NOT_FOUND;
    if (size > www_part->size) return ESP_ERR_INVALID_SIZE;
    if (xSemaphoreTake(www_lock, pdMS_TO_TICKS(10000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (www_mounted) { www_mounted = false; esp_vfs_littlefs_unregister(WWW_LABEL); }
    www_up.off = www_up.erased = 0; www_up.size = size;
    return ESP_OK;
}

static esp_err_t erase_next(void) {
    uint32_t len = www_part->size - www_up.erased;
    if (len > ERASE_STEP) len = ERASE_STEP;
    esp_err_t e = esp_partition_erase_range(www_part, www_up.erased, len);
    if (e == ESP_OK) www_up.erased += len;
    return e;
}

esp_err_t web_www_put(const uint8_t *p, size_t n) {
    if (www_up.off + n > www_up.size) return ESP_ERR_INVALID_SIZE;
    while (www_up.off + n > www_up.erased) { esp_err_t e = erase_next(); if (e != ESP_OK) return e; }
    esp_err_t e = esp_partition_write(www_part, www_up.off, p, n);
    if (e == ESP_OK) www_up.off += n;
    return e;
}

esp_err_t web_www_end(bool ok) {
    if (ok) {
        /* Leave the unused rest erased, as a full-size image flashed with
         * esptool would (LittleFS itself does not read unused blocks). */
        while (www_up.erased < www_part->size) {
            esp_err_t e = erase_next();
            if (e != ESP_OK) { ESP_LOGW(TAG, "erase at %" PRIu32 ": %s", www_up.erased, esp_err_to_name(e)); break; }
            vTaskDelay(1);
        }
    }
    esp_err_t e = www_mount();
    xSemaphoreGive(www_lock);
    return e;
}

/* ---- bridge side ---------------------------------------------------------- */
bool web_ws_connected(void) { return ws_fd >= 0; }

int web_ws_read(void *buf, size_t n, uint32_t wait_ms) {
    return (int)xStreamBufferReceive(ws_rx_sb, buf, n, pdMS_TO_TICKS(wait_ms));
}

bool web_ws_write(const void *buf, size_t n) {
    if (ws_fd < 0) return false;
    return eth_tx_enqueue(ETH_TX_WS, buf, n);
}

void web_start(void) {
    www_lock = xSemaphoreCreateMutex();
    ws_rx_sb = xStreamBufferCreate(WS_RX_BUF, 1);
    www_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, WWW_LABEL);
    if (!www_part) {
        /* Boards flashed with the older two-slot table (no "www" entry): the
         * region behind ota_1 is free, so register it at runtime on the main
         * flash. The bridge and the web app image then update over the LAN
         * without ever rewriting the partition table over USB. */
        esp_err_t e = esp_partition_register_external(NULL, WWW_FALLBACK_OFFSET, WWW_FALLBACK_SIZE, WWW_LABEL,
                                                      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, &www_part);
        if (e == ESP_OK) ESP_LOGI(TAG, "\"www\" not in the partition table, registered at 0x%x (%u KB)", WWW_FALLBACK_OFFSET, WWW_FALLBACK_SIZE / 1024);
        else { www_part = NULL; ESP_LOGW(TAG, "no \"www\" partition in the table and registering one failed: %s", esp_err_to_name(e)); }
    }
    www_mount();                                    /* failure is not fatal: the bridge works without the web app */

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = WEB_HTTP_PORT;
    cfg.core_id = 1;                                /* with eth_tx/tcp; the bridge loop keeps core 0 */
    cfg.stack_size = 10240;
    cfg.max_open_sockets = 8;                       /* parallel asset loads + the WebSocket; +3 internal (LWIP_MAX_SOCKETS 16) */
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.open_fn = on_open;
    cfg.close_fn = on_close;
    esp_err_t e = httpd_start(&hd, &cfg);
    if (e != ESP_OK) { ESP_LOGE(TAG, "httpd start: %s", esp_err_to_name(e)); return; }
    httpd_uri_t ws = {.uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true};
    httpd_uri_t any = {.uri = "/*", .method = HTTP_GET, .handler = file_get};
    httpd_register_uri_handler(hd, &ws);
    httpd_register_uri_handler(hd, &any);
    ESP_LOGI(TAG, "http on port %d, websocket /ws", WEB_HTTP_PORT);
}
