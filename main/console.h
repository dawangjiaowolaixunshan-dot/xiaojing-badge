#pragma once
/*
 * Setup over the USB serial port, one line each (tools/badge_setup.py):
 *   >ds_key=<key>   >bl_key=<key>   >wifi=<ssid>\t<password>
 *   >keys?          (prints which are set, never the values)
 *   >reboot
 */
void console_init_early(void);   /* first thing in app_main */
void console_start(void);        /* the command task, once the rest is up */
