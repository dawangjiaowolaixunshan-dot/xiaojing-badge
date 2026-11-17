#pragma once
/*
 * Setup over the USB serial port, one line each (tools/badge_setup.py):
 *   >ds_key=<key>   >bl_key=<key>   >wifi=<ssid>\t<password>
 *   >keys?          (prints which are set, never the values)
 *   >reboot
 */
void console_start(void);
