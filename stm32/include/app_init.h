/**
 * Top-level bring-up orchestration, called once from main before the
 * FreeRTOS scheduler starts. Keeps main.c trivially small.
 */

#ifndef APP_INIT_H
#define APP_INIT_H

/* Runs clock setup, on-board peripheral init, protocol-layer init, and
 * creates the FreeRTOS tasks. Never fails in a way the caller can recover
 * from -- any fatal path goes through the FreeRTOS hooks in main.c. */
void app_init(void);

#endif /* APP_INIT_H */
