/**
 * @file  console.h
 * @brief Interactive command dispatcher (preserves the ESP32 command set).
 */

#ifndef ESWCAP_CONSOLE_H
#define ESWCAP_CONSOLE_H

#include <stdbool.h>

/** Print the command list. */
void console_help(void);

/** Dispatch one line. Returns true if recognised. */
bool console_dispatch(const char *line);

/** True for commands that must interrupt an in-flight measurement. */
bool console_is_exclusive(const char *line);

/** Selected range for calibration/probe commands (0..3). */
uint8_t console_selected_range(void);

#endif /* ESWCAP_CONSOLE_H */
