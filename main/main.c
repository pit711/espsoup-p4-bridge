/* ESP32-P4 bridge for the on-module ESP32-C6 (Waveshare ESP32-P4-Module).
 *
 * The C6 hangs on SDIO (CLK 18, CMD 19, D0..D3 14..17, reset GPIO54).
 * At boot the C6 is probed through ESSL: an ESPsoup firmware answers INFO
 * and the P4 becomes a transparent byte pipe between the C6 and USB
 * (USB-Serial/JTAG), a TCP client on the Ethernet port, or a WebSocket
 * client of the web server (web.c, which also serves the ESPsoup web app
 * from the "www" partition on port 80).  Otherwise the C6 is assumed to run
 * ESP-Hosted and the P4 offers its OTA path instead.
 *
 * Lines starting with "P4 " are handled here and never forwarded:
 *   P4 INFO                 mode, C6 id/version, IP
 *   P4 OTA <size> <crc32>   -> "P4 READY", then <size> raw bytes follow
 *   P4 UPDATE <size> <crc32> same for the bridge firmware itself
 *   P4 WWW <size> <crc32>   same for the web app image (LittleFS, www partition)
 *   P4 RESET                reset the C6 and probe again
 * Everything else goes to the C6 unchanged; C6 output goes back to the side
 * that sent the last command. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdarg.h>
#include "driver/gpio.h"
#include "esp_check.h"
#include "driver/sdmmc_host.h"
#include "driver/usb_serial_jtag.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_rom_crc.h"
#include "esp_serial_slave_link/essl.h"
#include "esp_serial_slave_link/essl_sdio.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"
#include "sd_protocol_defs.h"
#include "eth.h"
#include "web.h"

static const char *TAG = "p4";
static const char build_id[] = __DATE__ " " __TIME__;
#define C6_RESET_GPIO 54
#define SDIO_CLK 18
#define SDIO_CMD 19
#define SDIO_D0 14
#define SDIO_D1 15
#define SDIO_D2 16
#define SDIO_D3 17
#define SLAVE_RX_BUF 512          /* must match the C6 receive buffer size */
#define RX_CHUNK 40960            /* one SDIO read; a whole CAP16 frame (24 B header + 32760 B) at once (stream mode) */
#define SDIO_BLOCK 512            /* CMD53 block size used by ESSL (essl_sdio.c, fixed) */
/* SDIO bus clock in kHz. The protocol layer leaves an SDIO card at 40 MHz
 * (sdmmc_io_enable_hs_mode caps it at SDMMC_FREQ_HIGHSPEED), so anything above
 * is requested again after card init in sdio_set_clock() and verified with a
 * CCCR read; on failure the link falls back to 40 MHz by itself. On the P4
 * the host driver can only make 40 MHz (PLL160M/4) or 50 MHz (SDIO PLL
 * 200 MHz/4) in this range; 50 MHz is the SDIO 2.0 high-speed limit the C6
 * slave advertises. Set SDMMC_FREQ_HIGHSPEED (40000) here to go back. */
#define SDIO_FREQ_KHZ SDMMC_FREQ_HIGHSPEED   /* 50000 measured 22.10.: no gain (32 KB read 2.91 ms vs 2.67 ms at 40 MHz) */

typedef enum { SINK_USB, SINK_TCP, SINK_WS } sink_t;
typedef enum { MODE_NONE, MODE_BRIDGE, MODE_HOSTED } mode_t;
static mode_t mode = MODE_NONE;
static sink_t sink = SINK_USB;
static essl_handle_t essl;
static sdmmc_card_t *card;
static bool hosted_up;
static char c6_id[80] = "?";
static uint8_t rxbuf[RX_CHUNK] __attribute__((aligned(64)));

/* Diagnostic trace of the C6 -> host path, one entry per SDIO read
 * (printed by "P4 STATS"): gap since the previous read finished, time in the
 * SDIO read, time handing the bytes to USB/TCP. */
typedef struct { uint32_t t_us, gap_us, read_us, send_us, bytes; } trace_t;
#define TRACE_N 64
static trace_t trace[TRACE_N];
static unsigned trace_n;
static int64_t trace_last;

/* ---- output to the active side ---------------------------------------- */
static void out(sink_t s, const void *p, size_t n) {
    if (s == SINK_TCP && eth_client_connected()) { eth_write(p, n); return; }
    if (s == SINK_WS && web_ws_connected()) { web_ws_write(p, n); return; }
    usb_serial_jtag_write_bytes(p, n, pdMS_TO_TICKS(1000));
}
static void outf(sink_t s, const char *fmt, ...) {
    char b[256]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    if (n > 0) out(s, b, n);
}
static int in_read(sink_t s, void *p, size_t n, uint32_t wait_ms) {
    if (s == SINK_TCP) return eth_read(p, n, wait_ms);
    if (s == SINK_WS) return web_ws_read(p, n, wait_ms);
    return usb_serial_jtag_read_bytes(p, n, pdMS_TO_TICKS(wait_ms));
}

/* ---- C6 reset and SDIO link -------------------------------------------- */
static void c6_reset(void) {
    gpio_config_t io = {.pin_bit_mask = 1ULL << C6_RESET_GPIO, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&io);
    gpio_set_level(C6_RESET_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(C6_RESET_GPIO, 1);
}

/* Ask the host for SDIO_FREQ_KHZ (the switch happens on the next transaction)
 * and check that a CCCR byte still reads back unchanged; otherwise return to
 * 40 MHz. A few attempts, as the SDIO PLL is only powered up on the switch. */
static void sdio_set_clock(void) {
    uint8_t cap0 = 0, cap1 = 0;
    if (SDIO_FREQ_KHZ <= SDMMC_FREQ_HIGHSPEED) return;   /* card init already set it */
    if (sdmmc_io_read_byte(card, 0, SD_IO_CCCR_CARD_CAP, &cap0) != ESP_OK) return;
    esp_err_t e = sdmmc_host_set_card_clk(card->host.slot, SDIO_FREQ_KHZ);
    if (e != ESP_OK) { ESP_LOGW(TAG, "set_card_clk %d kHz: %s, staying at %d kHz", SDIO_FREQ_KHZ, esp_err_to_name(e), card->real_freq_khz); return; }
    bool ok = false;
    for (int i = 0; i < 3 && !ok; i++)
        ok = sdmmc_io_read_byte(card, 0, SD_IO_CCCR_CARD_CAP, &cap1) == ESP_OK && cap1 == cap0;
    if (ok) card->max_freq_khz = SDIO_FREQ_KHZ;
    else {
        ESP_LOGW(TAG, "SDIO at %d kHz failed, back to %d kHz", SDIO_FREQ_KHZ, SDMMC_FREQ_HIGHSPEED);
        sdmmc_host_set_card_clk(card->host.slot, SDMMC_FREQ_HIGHSPEED);
        card->max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    }
    sdmmc_host_get_real_freq(card->host.slot, &card->real_freq_khz);
}

static uint32_t c6_pending;       /* bytes the C6 reported as queued that we have not read yet */

static esp_err_t sdio_up(void) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    /* Card init runs at 40 MHz (anything above makes sdmmc_card_init fail
     * with ESP_ERR_INVALID_ARG); sdio_set_clock() raises it afterwards. */
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    /* P4 DMA wants cache-line aligned buffers; let the driver bounce small
     * or odd-sized transfers through its own aligned buffer. */
    host.flags = SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = SDIO_CLK; slot.cmd = SDIO_CMD;
    slot.d0 = SDIO_D0; slot.d1 = SDIO_D1; slot.d2 = SDIO_D2; slot.d3 = SDIO_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    ESP_RETURN_ON_ERROR(sdmmc_host_init(), TAG, "host init");
    ESP_RETURN_ON_ERROR(sdmmc_host_init_slot(host.slot, &slot), TAG, "slot init");
    card = calloc(1, sizeof(*card));
    esp_err_t e = ESP_FAIL;
    for (int i = 0; i < 15 && e != ESP_OK; i++) {
        e = sdmmc_card_init(&host, card);
        if (e != ESP_OK) vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (e != ESP_OK) { ESP_LOGW(TAG, "card init: %s", esp_err_to_name(e)); return e; }
    sdio_set_clock();
    essl_sdio_config_t cfg = {.card = card, .recv_buffer_size = SLAVE_RX_BUF};
    ESP_RETURN_ON_ERROR(essl_sdio_init_dev(&essl, &cfg), TAG, "essl dev");
    ESP_RETURN_ON_ERROR(essl_init(essl, 1000), TAG, "essl init");
    c6_pending = 0;
    ESP_LOGI(TAG, "SDIO link up, %d kHz", card->real_freq_khz);
    return ESP_OK;
}

static void sdio_down(void) {
    if (essl) { essl_sdio_deinit_dev(essl); essl = NULL; }
    sdmmc_host_deinit();
    free(card); card = NULL;
}

/* Send to the C6; waits for slave buffers up to wait_ms. */
static bool c6_send(const void *p, size_t n, uint32_t wait_ms) {
    /* The SDMMC host DMAs from the buffer: internal RAM, word aligned. */
    static uint8_t txcopy[1024] __attribute__((aligned(64)));
    int64_t deadline = esp_timer_get_time() + (int64_t)wait_ms * 1000;
    const uint8_t *s = p;
    while (n) {
        size_t k = n > sizeof(txcopy) ? sizeof(txcopy) : n;
        memcpy(txcopy, s, k);
        esp_err_t e = essl_send_packet(essl, txcopy, k, 50);
        if (e == ESP_OK) { s += k; n -= k; continue; }
        if (e != ESP_ERR_NOT_FOUND && e != ESP_ERR_TIMEOUT) { ESP_LOGW(TAG, "send: %s", esp_err_to_name(e)); return false; }
        if (esp_timer_get_time() > deadline) return false;
        vTaskDelay(1);
    }
    return true;
}

/* Read whatever the C6 has queued (0 if nothing). Every SDIO transaction
 * costs ~140 us before the first data bit, so: whole 512-byte blocks go in
 * one multi-block CMD53 and a tail shorter than a block stays in the slave
 * until the next poll (it is read on its own only when nothing follows), and
 * while c6_pending still holds a full block the length register is not read
 * again (ESSL serves the read from its cached count). */
static int c6_recv(uint8_t *buf, size_t cap) {
    uint32_t avail = c6_pending;
    if (avail < SDIO_BLOCK && (essl_get_rx_data_size(essl, &avail, 10) != ESP_OK || !avail)) { c6_pending = 0; return 0; }
    size_t n = avail > cap ? cap : avail;
    if (n >= SDIO_BLOCK) n &= ~(size_t)(SDIO_BLOCK - 1);
    size_t got = 0;
    esp_err_t e = essl_get_packet(essl, buf, n, &got, 50);
    if (e != ESP_OK && e != ESP_ERR_NOT_FINISHED) { c6_pending = 0; ESP_LOGW(TAG, "recv: %s", esp_err_to_name(e)); return 0; }
    c6_pending = avail - got;
    return (int)got;
}

/* Collect C6 output until a line containing `token` arrives or the timeout
 * expires; everything collected is forwarded to `s`. */
static bool c6_wait_line(sink_t s, const char *token, uint32_t ms, char *line, size_t cap) {
    int64_t deadline = esp_timer_get_time() + (int64_t)ms * 1000;
    size_t used = 0;
    while (esp_timer_get_time() < deadline) {
        int n = c6_recv(rxbuf, sizeof(rxbuf));
        if (!n) { vTaskDelay(1); continue; }
        out(s, rxbuf, n);
        for (int i = 0; i < n; i++) {
            char c = rxbuf[i];
            if (c == '\n') {
                line[used] = 0;
                if (strstr(line, token)) return true;
                if (!strncmp(line, "ERR", 3)) return false;
                used = 0;
            } else if (used < cap - 1) line[used++] = c;
        }
    }
    return false;
}

static bool probe_espsoup(void) {
    char line[96];
    for (int t = 0; t < 3; t++) {
        if (!c6_send("INFO\n", 5, 300)) continue;
        if (c6_wait_line(SINK_USB, "burst", 400, line, sizeof(line))) {
            strlcpy(c6_id, line, sizeof(c6_id));
            return true;
        }
    }
    return false;
}

/* ---- ESP-Hosted side ----------------------------------------------------- */
/* esp_hosted_deinit() is not reliable mid-connection; a reboot is, and the
 * bridge probes for ESPsoup before touching ESP-Hosted anyway. */
static void hosted_down(void) {
    if (!hosted_up) return;
    ESP_LOGI(TAG, "restarting to leave ESP-Hosted");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}
static bool hosted_bring_up(void) {
    if (!hosted_up) {
        if (esp_hosted_init() != ESP_OK) return false;
        hosted_up = true;
    }
    if (esp_hosted_connect_to_slave() != ESP_OK) return false;
    esp_hosted_coprocessor_fwver_t v = {0};
    if (esp_hosted_get_coprocessor_fwversion(&v) == ESP_OK)
        snprintf(c6_id, sizeof(c6_id), "esp-hosted %lu.%lu.%lu", (unsigned long)v.major1, (unsigned long)v.minor1, (unsigned long)v.patch1);
    else strlcpy(c6_id, "esp-hosted (old, no version)", sizeof(c6_id));
    return true;
}

static volatile bool detecting;
static void detect_mode(void) {
    detecting = true;
    hosted_down();
    c6_reset();
    vTaskDelay(pdMS_TO_TICKS(800));
    mode = MODE_NONE;
    if (sdio_up() == ESP_OK && probe_espsoup()) { mode = MODE_BRIDGE; ESP_LOGI(TAG, "C6 runs ESPsoup: %s", c6_id); }
    else {
        sdio_down();
        if (hosted_bring_up()) { mode = MODE_HOSTED; ESP_LOGI(TAG, "C6 runs %s", c6_id); }
        else ESP_LOGW(TAG, "C6 not responding");
    }
    detecting = false;
}
static void detect_task(void *arg) { detect_mode(); vTaskDelete(NULL); }
static void detect_async(void) {
    if (detecting) return;
    detecting = true;
    xTaskCreatePinnedToCore(detect_task, "detect", 8192, NULL, 4, NULL, 1);
}
static const char *mode_name(void) { return detecting ? "detecting" : mode == MODE_BRIDGE ? "bridge" : mode == MODE_HOSTED ? "hosted" : "none"; }

/* ---- chunked upload: USB/TCP -> P4 -------------------------------------- */
/* Stop-and-wait per chunk: the sender waits for "P4 ACK" before the next
 * one, so the USB receive buffer never overflows. Each chunk goes to `put`;
 * errors are reported to `s` and end the upload. */
typedef esp_err_t (*chunk_put_t)(void *ctx, const uint8_t *p, size_t n);
static bool recv_chunks(sink_t s, uint32_t size, uint32_t crc_want, chunk_put_t put, void *ctx) {
    static uint8_t chunk[1400];
    uint32_t crc = 0, done = 0;
    outf(s, "P4 READY %u\n", (unsigned)sizeof(chunk));
    while (done < size) {
        size_t want = size - done > sizeof(chunk) ? sizeof(chunk) : size - done;
        size_t got = 0;
        int64_t deadline = esp_timer_get_time() + 5000000;
        while (got < want && esp_timer_get_time() < deadline) {
            int n = in_read(s, chunk + got, want - got, 200);
            if (n > 0) got += n;
        }
        if (got < want) { outf(s, "P4 ERR timeout at %" PRIu32 "\n", done + (uint32_t)got); return false; }
        crc = esp_rom_crc32_le(crc, chunk, want);
        esp_err_t e = put(ctx, chunk, want);
        if (e != ESP_OK) { outf(s, "P4 ERR write %s at %" PRIu32 "\n", esp_err_to_name(e), done); return false; }
        done += want;
        outf(s, "P4 ACK %" PRIu32 "\n", done);
    }
    if (crc != crc_want) { outf(s, "P4 ERR crc %08" PRIx32 " != %08" PRIx32 "\n", crc, crc_want); return false; }
    return true;
}

/* ---- OTA: USB/TCP -> C6 ------------------------------------------------- */
static esp_err_t put_hosted(void *ctx, const uint8_t *p, size_t n) { (void)ctx; return esp_hosted_slave_ota_write((uint8_t *)p, n); }
static esp_err_t put_c6(void *ctx, const uint8_t *p, size_t n) { (void)ctx; return c6_send(p, n, 5000) ? ESP_OK : ESP_FAIL; }

static void ota_stream(sink_t s, uint32_t size, uint32_t crc_want) {
    esp_err_t e = ESP_OK;
    char line[96];
    bool via_hosted = mode == MODE_HOSTED;
    if (via_hosted) {
        e = esp_hosted_slave_ota_begin();
        if (e != ESP_OK) { outf(s, "P4 ERR ota_begin %s\n", esp_err_to_name(e)); return; }
    } else {
        char cmd[48]; int n = snprintf(cmd, sizeof(cmd), "OTA %" PRIu32 " %08" PRIx32 "\n", size, crc_want);
        if (!c6_send(cmd, n, 1000) || !c6_wait_line(s, "OK OTA", 3000, line, sizeof(line))) { outf(s, "P4 ERR c6_ota_begin\n"); return; }
    }
    if (!recv_chunks(s, size, crc_want, via_hosted ? put_hosted : put_c6, NULL)) return;
    if (via_hosted) {
        e = esp_hosted_slave_ota_end();
        if (e != ESP_OK) { outf(s, "P4 ERR ota_end %s\n", esp_err_to_name(e)); return; }
        e = esp_hosted_slave_ota_activate();
        outf(s, "P4 OK ota %" PRIu32 " bytes, activate %s\n", size, esp_err_to_name(e));
    } else {
        bool ok = c6_wait_line(s, "OK OTA DONE", 15000, line, sizeof(line));
        outf(s, "P4 %s ota %" PRIu32 " bytes\n", ok ? "OK" : "ERR", size);
        if (!ok) return;
    }
    vTaskDelay(pdMS_TO_TICKS(1500));
    outf(s, "P4 RESET\n");
    if (mode == MODE_BRIDGE) sdio_down();
    detect_mode();
    outf(s, "P4 MODE %s %s\n", mode_name(), c6_id);
}

/* ---- self-update: USB/TCP -> the other P4 OTA slot ---------------------- */
static esp_err_t put_ota(void *ctx, const uint8_t *p, size_t n) { return esp_ota_write(*(esp_ota_handle_t *)ctx, p, n); }

static void self_update(sink_t s, uint32_t size, uint32_t crc_want) {
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next) { outf(s, "P4 ERR no_ota_partition\n"); return; }
    if (size < 4096 || size > next->size) { outf(s, "P4 ERR size\n"); return; }
    esp_ota_handle_t h;
    esp_err_t e = esp_ota_begin(next, size, &h);
    if (e != ESP_OK) { outf(s, "P4 ERR ota_begin %s\n", esp_err_to_name(e)); return; }
    if (!recv_chunks(s, size, crc_want, put_ota, &h)) { esp_ota_abort(h); return; }
    e = esp_ota_end(h);
    if (e == ESP_OK) e = esp_ota_set_boot_partition(next);
    if (e != ESP_OK) { outf(s, "P4 ERR ota_end %s\n", esp_err_to_name(e)); return; }
    outf(s, "P4 OK update %" PRIu32 " bytes -> %s, restarting\n", size, next->label);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

/* ---- web app image: USB/TCP/WS -> www partition (LittleFS) -------------- */
static esp_err_t put_www(void *ctx, const uint8_t *p, size_t n) { (void)ctx; return web_www_put(p, n); }

static void www_update(sink_t s, uint32_t size, uint32_t crc_want) {
    uint32_t cap = web_www_size();
    if (!cap) { outf(s, "P4 ERR no_www_partition\n"); return; }
    if (size < 8192 || size > cap) { outf(s, "P4 ERR size (8192..%" PRIu32 ")\n", cap); return; }
    esp_err_t e = web_www_begin(size);           /* unmounts; the web server answers 503 meanwhile */
    if (e != ESP_OK) { outf(s, "P4 ERR www_begin %s\n", esp_err_to_name(e)); return; }
    int64_t t0 = esp_timer_get_time();
    bool ok = recv_chunks(s, size, crc_want, put_www, NULL);
    e = web_www_end(ok);                         /* erases the rest, remounts */
    if (!ok) return;                             /* recv_chunks reported the error */
    if (e != ESP_OK) { outf(s, "P4 ERR www mount %s\n", esp_err_to_name(e)); return; }
    outf(s, "P4 OK www %" PRIu32 " bytes in %" PRIu32 " ms, mounted\n", size, (uint32_t)((esp_timer_get_time() - t0) / 1000));
}

static void local_command(sink_t s, char *line) {
    uint32_t size, crc;
    if (!strcmp(line, "P4 INFO")) {
        outf(s, "P4 mode=%s c6=\"%s\" ip=%s tcp=%d http=%d www=%s ws=%d sdio=%d kHz heap=%u build=\"%s\"\n", mode_name(), c6_id, eth_ip(), ETH_TCP_PORT,
             WEB_HTTP_PORT, web_www_mounted() ? "mounted" : "none", web_ws_connected(),
             (!detecting && card) ? card->real_freq_khz : 0, (unsigned)esp_get_free_heap_size(), build_id);
    } else if (sscanf(line, "P4 UPDATE %" SCNu32 " %" SCNx32, &size, &crc) == 2) {
        /* Never touches the C6, so it stays available while detection hangs
         * (e.g. ESP-Hosted waiting for a slave that runs ESPsoup). */
        self_update(s, size, crc);
    } else if (sscanf(line, "P4 WWW %" SCNu32 " %" SCNx32, &size, &crc) == 2) {
        www_update(s, size, crc);                /* same: only the flash and the web server are involved */
    } else if (detecting) {
        outf(s, "P4 ERR detecting\n");
    } else if (sscanf(line, "P4 OTA %" SCNu32 " %" SCNx32, &size, &crc) == 2) {
        ota_stream(s, size, crc);
    } else if (!strcmp(line, "P4 STATS")) {
        unsigned first = trace_n > TRACE_N ? trace_n - TRACE_N : 0;
        uint32_t t0 = trace[first % TRACE_N].t_us;
        outf(s, "P4 STATS t_ms gap_us read_us send_us bytes\n");
        for (unsigned i = first; i < trace_n; i++) {
            trace_t *e = &trace[i % TRACE_N];
            outf(s, "%8.3f %6" PRIu32 " %6" PRIu32 " %6" PRIu32 " %6" PRIu32 "\n", (e->t_us - t0) / 1000.0, e->gap_us, e->read_us, e->send_us, e->bytes);
        }
        outf(s, "P4 STATS END\n");
    } else if (sscanf(line, "P4 BLAST %" SCNu32, &size) == 1) {
        /* Diagnostic: the USB/TCP path alone, `size` bytes of filler without SDIO. */
        memset(rxbuf, 'x', sizeof(rxbuf));
        while (size) { size_t k = size > sizeof(rxbuf) ? sizeof(rxbuf) : size; out(s, rxbuf, k); size -= k; }
    } else if (!strcmp(line, "P4 RESET")) {
        if (mode == MODE_BRIDGE) sdio_down();
        detect_mode();
        outf(s, "P4 MODE %s %s\n", mode_name(), c6_id);
    } else outf(s, "P4 ERR command\n");
}

/* Mirror the log on the USB port as "# ..." lines (UART0 keeps it too). */
static int usb_log(const char *fmt, va_list ap) {
    char b[200];
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(b, sizeof(b), fmt, ap2);
    va_end(ap2);
    if (n > 0) {
        if (n > (int)sizeof(b) - 1) n = sizeof(b) - 1;
        usb_serial_jtag_write_bytes("# ", 2, 0);
        usb_serial_jtag_write_bytes(b, n, 0);
    }
    return vprintf(fmt, ap);
}

/* Line parser per input side; complete lines are forwarded or handled. */
typedef struct { char buf[256]; size_t used; } parser_t;
static void feed(sink_t s, parser_t *p, const uint8_t *d, int n) {
    for (int i = 0; i < n; i++) {
        char c = d[i];
        if (c != '\n') { if (p->used < sizeof(p->buf) - 2) p->buf[p->used++] = c; continue; }
        p->buf[p->used] = 0;
        sink = s;
        if (!strncmp(p->buf, "P4 ", 3) || !strcmp(p->buf, "P4")) local_command(s, p->buf);
        else if (mode == MODE_BRIDGE && !detecting) {
            p->buf[p->used++] = '\n';
            if (!c6_send(p->buf, p->used, 1000)) outf(s, "P4 ERR c6_busy\n");
        } else outf(s, "P4 ERR no_bridge (mode %s)\n", mode_name());
        p->used = 0;
    }
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    usb_serial_jtag_driver_config_t usb = {.tx_buffer_size = 16384, .rx_buffer_size = 4096};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    esp_log_set_vprintf(usb_log);
    ESP_LOGI(TAG, "p4bridge start");
    eth_start();
    web_start();
    detect_async();

    static parser_t pu, pt, pw;
    static uint8_t in[1024];
    int64_t active_until = 0;
    for (;;) {
        bool busy = false;
        int n = usb_serial_jtag_read_bytes(in, sizeof(in), 0);
        if (n > 0) { feed(SINK_USB, &pu, in, n); busy = true; }
        n = eth_read(in, sizeof(in), 0);
        if (n > 0) { feed(SINK_TCP, &pt, in, n); busy = true; }
        n = web_ws_read(in, sizeof(in), 0);
        if (n > 0) { feed(SINK_WS, &pw, in, n); busy = true; }
        if (mode == MODE_BRIDGE && !detecting) {
            int64_t t0 = esp_timer_get_time();
            n = c6_recv(rxbuf, sizeof(rxbuf));
            if (n > 0) {
                int64_t t1 = esp_timer_get_time();
                out(sink, rxbuf, n);
                int64_t t2 = esp_timer_get_time();
                trace_t *e = &trace[trace_n++ % TRACE_N];
                e->t_us = (uint32_t)t0; e->gap_us = (uint32_t)(t0 - trace_last);
                e->read_us = (uint32_t)(t1 - t0); e->send_us = (uint32_t)(t2 - t1); e->bytes = (uint32_t)n;
                trace_last = t2;
                busy = true;
            }
        }
        /* While a transfer is running, poll the SDIO link tightly: a tick of
         * sleep per handover was the throughput limit. Sleep again once the
         * link has been quiet for a while. */
        if (busy) active_until = esp_timer_get_time() + 200000;
        else if (esp_timer_get_time() < active_until) taskYIELD();
        else vTaskDelay(1);
    }
}
