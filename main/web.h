#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* HTTP server on WEB_HTTP_PORT: the ESPsoup web app from the "www" LittleFS
 * partition (read-only), and a WebSocket "/ws" that carries the raw C6 byte
 * stream. One WebSocket client at a time; its bytes land in a stream buffer
 * the bridge drains, output goes through the shared sender task of eth.c. */
#define WEB_HTTP_PORT 80

void web_start(void);                                   /* mount www (may fail, non-fatal) and start httpd */
bool web_www_mounted(void);
bool web_ws_connected(void);
int  web_ws_read(void *buf, size_t n, uint32_t wait_ms); /* bytes from the WebSocket client */
bool web_ws_write(const void *buf, size_t n);            /* to the WebSocket client (queued, see eth.c) */
bool web_ws_send(const void *buf, size_t n);             /* one binary frame, called by the sender task in eth.c */

/* "P4 WWW": replace the LittleFS image in the www partition. begin() unmounts
 * and blocks the file handler, put() writes sequentially (erasing ahead in
 * 64 KB blocks), end() erases the remainder, remounts and releases. */
esp_err_t web_www_begin(uint32_t size);
esp_err_t web_www_put(const uint8_t *p, size_t n);
esp_err_t web_www_end(bool ok);
uint32_t  web_www_size(void);                            /* partition size, 0 if absent */
