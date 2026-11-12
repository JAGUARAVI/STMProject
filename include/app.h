/**
 * @file  app.h
 * @brief Application entry points.
 */

#ifndef ESWCAP_APP_H
#define ESWCAP_APP_H

/** Initialise every subsystem. Safe-boot state: stopped, automation flags off,
 *  front end idle — nothing charges until the operator asks. */
void app_init(void);

/** Run the cooperative main loop. Never returns. */
void app_run(void);

#endif /* ESWCAP_APP_H */
