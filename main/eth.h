#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Ethernet (IP101 on RMII, DHCP) and a raw TCP server on ETH_TCP_PORT.
 * One client at a time; its bytes land in a stream buffer the bridge drains.
 * The sender task here also serves the WebSocket client of web.c: both
 * share one 128 KB outgoing message buffer, each message is tagged with its
 * destination. */
#define ETH_TCP_PORT 5555
enum { ETH_TX_TCP = 1, ETH_TX_WS = 2 };

void eth_start(void);
const char *eth_ip(void);              /* "0.0.0.0" until DHCP is done */
bool eth_client_connected(void);
int eth_read(void *buf, size_t n, uint32_t wait_ms);   /* bytes from the TCP client */
bool eth_write(const void *buf, size_t n);              /* to the TCP client (= eth_tx_enqueue(ETH_TX_TCP, ...)) */
bool eth_tx_enqueue(uint8_t to, const void *buf, size_t n);  /* queue for the sender task; only copies */
bool eth_tx_idle(void);                                 /* nothing queued for the sender task */
