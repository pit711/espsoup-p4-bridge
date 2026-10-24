# ESPsoup P4 bridge

**The ESP32-C6 inside a Waveshare ESP32-P4 module as a receive-only SDR, streamed over Ethernet at ~10 MB/s.**

The Waveshare ESP32-P4 modules carry an ESP32-C6 that normally acts as the P4's Wi-Fi coprocessor (ESP-Hosted over SDIO). This project turns the pair around: the C6 runs [ESPsoup](https://github.com/pit711/espsoup-firmware) (receive-only ESP-SDR firmware) and the P4 becomes a transparent bridge between the C6 and

- **Ethernet**: TCP port `5555`, plus a web server on port `80` that serves the ESPsoup web app and streams the C6 over a WebSocket (`/ws`),
- **USB**: the P4's USB-Serial/JTAG port.

The C6 speaks the same line protocol as on any ESPsoup board (`INFO`, `CAPS`, `FREQ`, `CAP16`, `SPEC`, `RXRUN`, ...), so existing tools work unchanged. They just reach it through a network socket instead of a COM port.

```
 antenna ── ESP32-C6 ══ SDIO 4-bit, 40 MHz ══ ESP32-P4 ──┬── Ethernet 100 Mbit: TCP 5555 / HTTP 80 + WebSocket
           (ESPsoup)                          (bridge)    └── USB-Serial/JTAG
```

> Status: tested on the Waveshare **ESP32-P4-Module-DEV-KIT** (P4NRW32 rev. 1.3, C6FH8). The ESP32-P4-WIFI6-DEV-KIT is electrically the same board but untested.

## What you need

| <a href="https://espsoup.com/go/p4-devkit"><img src="https://espsoup.com/amzimg/p4-devkit" width="170" alt="Waveshare ESP32-P4-Module-DEV-KIT"></a> |
|:---:|
| **Waveshare ESP32-P4-Module-DEV-KIT** ⭐ |
| ESP32-P4 + on-module ESP32-C6, 100 Mbit RJ45 — the board this project is tested on |
| [View on Amazon](https://espsoup.com/go/p4-devkit) |

Plus a USB-C data cable and an Ethernet cable. The kit variants -A/-B/-C only add a camera or a display, which the bridge doesn't use.

<sub>Affiliate link (ad). As an Amazon Associate I earn from qualifying purchases — it costs you nothing extra and keeps the soup cooking.
Product photo comes from Amazon.</sub>

## Flashing

Plug a USB-C cable into the socket labelled **USB**, not "USB TO UART". The flasher writes the bridge to the P4 through its ROM bootloader. It then sends ESPsoup to the C6 *through the bridge*, so the C6 never needs a cable of its own. A factory board, whose C6 still runs ESP-Hosted, works the same way: the bridge detects ESP-Hosted and uses its OTA path.

### Web flasher (Chrome or Edge)

**<https://pit711.github.io/espsoup-p4-bridge/flasher/>**

Pick *P4 bridge + ESPsoup on the C6*, click **Connect and flash** and choose the "USB JTAG/serial debug unit" port. The flasher can also update only the P4 or only the C6, or put ESP-Hosted back on the C6 to get Wi-Fi again. Offline it runs the same from a clone: `python -m http.server 8000` in the repository root, then <http://localhost:8000/flasher/> (Web Serial needs `https://` or `localhost`).

### Command line

```
pip install esptool pyserial
python tools/flash.py -p COM21              # everything (Linux: /dev/ttyACM0)
python tools/flash.py -p COM21 --only-c6    # bridge already installed
python tools/flash.py -p COM21 --c6 firmware/c6/esp-hosted-2.13.0-c6-sdio.bin   # back to Wi-Fi
```

After that, plug in Ethernet. The bridge gets its address by DHCP and shows it in `P4 INFO`.

### Images in `firmware/`

| File | Goes to | Notes |
|---|---|---|
| `p4/bootloader.bin` | P4 `0x2000` | |
| `p4/partition-table.bin` | P4 `0x8000` | two 2 MB app slots + 6 MB `www` |
| `p4/ota_data_initial.bin` | P4 `0xf000` | |
| `p4/p4bridge.bin` | P4 `0x20000` | this repository |
| `c6/espsoup-c6-sdio.bin` | C6, through the bridge | ESPsoup, profile `esp32c6-sdio`, GPL-3.0-or-later |
| `c6/esp-hosted-2.13.0-c6-sdio.bin` | C6, through the bridge | Espressif ESP-Hosted slave, Apache-2.0, to undo everything |

`manifest.json` lists the same files for both flashers.

## Using it

```
python tools/p4tool.py -t <ip> info              # bridge mode, C6 firmware, IP, SDIO clock
python tools/p4tool.py -t <ip> cmd INFO          # any ESPsoup command, replies printed
python tools/p4tool.py -t <ip> stream 10         # RXRUN throughput
python tools/p4tool.py -t <ip> bench 10 2        # CAP16 throughput, one command queued ahead
python tools/p4tool.py -t <ip> ota  <c6.bin>     # update the C6 over the LAN
python tools/p4tool.py -t <ip> update <p4bridge.bin>   # update the bridge itself over the LAN
python tools/p4tool.py -t <ip> www  build/www.bin      # upload a web app image
```

Use `-p COM21` instead of `-t <ip>` to go over USB. Once the first flash is done, everything goes over the network.

### Bridge commands

Lines starting with `P4 ` are handled by the bridge and never forwarded. Everything else goes to the C6 unchanged, and C6 output goes back to whichever side (USB, TCP or WebSocket) sent the last command.

| Command | Effect |
|---|---|
| `P4 INFO` | mode (`bridge` / `hosted` / `detecting`), C6 id, IP, ports, `www` state, SDIO clock, heap, build |
| `P4 OTA <size> <crc32>` | flash the C6 (ESPsoup or ESP-Hosted), then probe again → `P4 MODE ...` |
| `P4 UPDATE <size> <crc32>` | write the bridge into the other P4 app slot and restart |
| `P4 WWW <size> <crc32>` | write a LittleFS image into the `www` partition and mount it |
| `P4 RESET` | reset the C6 and probe again |
| `P4 STATS` | trace of the last 64 SDIO reads (gap, read time, send time, bytes) |
| `P4 BLAST <bytes>` | filler data straight from the P4: measures the network path alone |

Uploads are stop-and-wait. After `P4 READY <chunk>` the sender sends one chunk and waits for `P4 ACK <offset>` before the next, so the USB receive buffer can never overflow. Bridge log lines start with `# `.

### Web app

`http://<ip>/` serves whatever is in the `www` partition. Files are stored gzipped (`<path>.gz`, sent with `Content-Encoding: gzip`). A WebSocket on `/ws` carries the raw protocol bytes in binary frames, at the same rate as TCP. To build an image:

```
python tools/make_www.py <dir with index.html>   # gzip + checks -> ./www
idf.py build                                     # -> build/www.bin (LittleFS)
python tools/p4tool.py -t <ip> www build/www.bin
```

`tools/sample-site/` is a small placeholder page for testing. The bridge runs fine without a web app.

## Performance

Measured over 100 Mbit Ethernet with 32 KB frames (`CAP16 16380`, 80 MS/s capture), C6 at 40 MHz SDIO:

| Test | Result |
|---|---|
| `stream` (RXRUN, frames back to back) | **10.0 MB/s**, 600 frames CRC-checked, 0 errors |
| same over the WebSocket | 9.7 MB/s |
| `bench … 2` (one command queued ahead) | 6.1 MB/s |
| `bench` (one round trip per frame) | 3.7 MB/s |
| `SPEC 5000 1 1 0 0 256` (ring capture) | 878 frames, 0 drops, 176 frames/s |
| `P4 BLAST` (network path alone) | 11.85 MB/s = the 100 Mbit line |
| round trip `P4 INFO` / `INFO` to the C6 | 0.94 ms / 1.05 ms |
| same SDR over the P4's USB-Serial/JTAG | 0.78 MB/s (USB is the limit, not SDIO) |

For comparison, the best public P4↔C6 SDIO figure we found is ~36–40 Mbit/s TCP with ESP-Hosted.

## How it got from 1 to 10 MB/s

The first version moved 1.06 MB/s over Ethernet. These are the steps that mattered, in order:

| Step | `stream` |
|---|---|
| Start: SDIO packet mode, 1 ms ticks on both chips, 4 KB reads | 1.06 MB/s* |
| C6: **stream mode**: every pending send buffer is visible to the host as one chain; tight polling while traffic flows | 2.98 |
| P4: **32 KB reads** in one go, tight polling, 64 KB lwIP send buffer | 4.68 |
| C6: **a frame is one chain** (header + payload held back until complete, `sdio_link_hold`) | 7.59 |
| C6: **capture CPU work halved**: sentinels on a 64-sample grid, IQ8 packing and CRC32 (slicing-by-4, bit-identical to the ROM CRC) in one loop, PHY I2C reads only when needed: ~4.0 → ~2.4 ms per frame | 7.51 |
| P4: **separate sender task** on core 1: a 128 KB stream buffer decouples SDIO reads from lwIP, so the next read never waits for TCP | 9.41 |
| C6: under hold, **wait for the buffers the host is reading** instead of giving up the hold | **10.07** |

\* measured with one round trip per frame. At that stage it was the only test available.

The small things that also count:

- **Poll tight, but only while it matters.** Both chips spin while data flowed in the last 200 ms and fall back to a 1 ms tick otherwise. A `vTaskDelay(1)` in a hot loop costs a whole millisecond per frame.
- **Read in multiples of 512.** SDIO reads are whole 512-byte blocks (up to 40 KB at once), and the rest waits for the next poll. The length register is read only when nothing known is pending.
- **The SDIO clock is not the bottleneck.** 50 MHz runs without errors and gains nothing. A 32 KB read takes 2.7 ms: 1.64 ms of data phase plus a ~16 µs gap per 512-byte block on the slave side. That gap does not depend on the clock or the descriptor size, and it now sets the frame period (3.0 ms). The bridge stays at 40 MHz.
- **`-O2`** (`CONFIG_COMPILER_OPTIMIZATION_PERF`): the per-transaction software cost of the SDMMC host driver shows at these rates.
- **Pure polling on the C6.** The SDIO slave transport runs on the HAL without an ISR, so it also works inside the interrupt-locked ring-capture path (`SPEC`).
- **Serve the web app from the flash cache.** LittleFS is mounted with `CONFIG_LITTLEFS_MMAP_PARTITION`. `esp_partition_read` would stall both cores on every read and drag the stream down.
- **Store the web app gzipped and compact.** The LittleFS image is built with `--compact`, so its 0xFF tail can be skipped: a 6 MB partition uploads only the ~2.6 MB that is used, in ~12 s.

## Pitfalls we hit

- **Who owns the SDMMC host?** ESP-Hosted starts itself from a constructor and grabs the SDMMC host. `CMakeLists.txt` drops that file, so the bridge can probe for ESPsoup first and only then call `esp_hosted_init()` itself.
- **ESP-Hosted needs room for its SDIO buffers.** Next to the web server and the 128 KB send buffer, its double buffer no longer fits into internal DMA RAM. `ESP_ERROR_CHECK` fails in `process_init_event`, and the P4 ends up in a reboot loop on any board whose C6 still runs ESP-Hosted. The fix is `CONFIG_SPIRAM` with `CONFIG_SPIRAM_USE_CAPS_ALLOC` (PSRAM only on request, so the bridge's hot path stays internal) and `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM`. The throughput is unchanged.
- **Leaving ESP-Hosted means rebooting the P4.** `esp_hosted_deinit()` is not reliable mid-connection, so after flashing a C6 that ran ESP-Hosted, the bridge restarts. Both flashers reconnect on their own.
- **`max_freq_khz` above 40000 before card init** gives `ESP_ERR_INVALID_ARG`. Set the clock after the card is up.
- **ESP-IDF 6.2 WebSocket handler:** the URI handler is not called on the handshake. The first data frame adopts the client instead (`ws_adopt` in `web.c`).
- **DTR/RTS on the P4's USB-Serial/JTAG reset the chip.** Keep both low when opening the port, as `p4tool.py` and the web flasher do.
- **P4 revision 1.3** needs `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` and `CONFIG_ESP32P4_REV_MIN_100`. Both are in `sdkconfig.defaults`.
- **No `www` partition** (older partition table)? The bridge registers the region at runtime, so `P4 UPDATE` + `P4 WWW` work without USB.
- **PoE:** the board has no PoE stage of its own. Header H4 only brings out the RJ45 centre taps. **Never put a jumper on H4**, because it shorts the 48 V.
- **Only one C6 path is reachable from outside:** UART0 and IO2/IO8/IO9. More wires do not make a faster link. The 4-pin header (IO9 / GND / RX / TX) is the emergency flashing path for the C6 (IO9 to GND, hold BOOT).
- **No Wi-Fi while the C6 is an SDR.** The module talks over Ethernet only. Flash ESP-Hosted back to get Wi-Fi.

## Hardware notes

| | |
|---|---|
| P4 ↔ C6 | SDIO 4-bit: CLK 18, CMD 19, D0–D3 14–17, C6 reset = P4 GPIO54 |
| Ethernet | internal EMAC, RMII, IP101: MDC 31, MDIO 52, REF_CLK in 50, PHY reset 51 |
| USB-C "USB" | P4 USB-Serial/JTAG: data port of the bridge |
| USB-C "USB TO UART" | CH343 on P4 UART0 (GPIO37/38): bridge logs |

## Building

ESP-IDF 6.2 (target `esp32p4`):

```
idf.py build                          # build/p4bridge.bin, plus build/www.bin from ./www
python tools/p4tool.py -t <ip> update build/p4bridge.bin     # over the LAN
idf.py -p COM21 flash                 # or the first time over USB
```

The C6 side is the `esp32c6-sdio` profile of [espsoup-firmware](https://github.com/pit711/espsoup-firmware), branch [`p4-sdio`](https://github.com/pit711/espsoup-firmware/tree/p4-sdio) (SDIO slave transport, in-band OTA, faster frames).

## License

The bridge is licensed GPL-3.0-or-later (see `LICENSE`), like the ESPsoup firmware it talks to. The ESPsoup C6 image in `firmware/c6/` is GPL-3.0-or-later and was built from espsoup-firmware commit [`356b5ae`](https://github.com/pit711/espsoup-firmware/commit/356b5ae) (branch `p4-sdio`). The ESP-Hosted image is © Espressif, Apache-2.0. ESP-SDR is by [ESPARGOS](https://github.com/ESPARGOS/esp-sdr). esptool-js is loaded from jsDelivr by the web flasher.
