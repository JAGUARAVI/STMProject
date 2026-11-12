/**
 * @file  bsp_console.h
 * @brief USART1 line-oriented console with interrupt-driven RX.
 */

#ifndef ESWCAP_BSP_CONSOLE_H
#define ESWCAP_BSP_CONSOLE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/** Configure USART1 (PA9/PA10, 115200 8N1) and the RX ring. */
bool console_init(void);

/** Pump the receiver: call frequently from the main loop. Moves received
 *  bytes into the line buffer and dispatches complete lines via the
 *  registered handler. */
void console_poll(void);

/** True when an entire command line is ready. */
bool console_line_ready(void);

/** Copy the pending line (without CR/LF) into buf. Returns false if none. */
bool console_read_line(char *buf, size_t len);

/** Blocking write helpers. */
void console_write(const char *s);
void console_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** True if a byte is waiting (used by exclusive commands to detect an abort). */
bool console_rx_pending(void);

/** Discard pending input. */
void console_flush_rx(void);

/** Raw single-byte write, used by the watcher/telemetry path. */
void console_putc(char c);

#endif /* ESWCAP_BSP_CONSOLE_H */
