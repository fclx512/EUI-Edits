#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int eui_tray_init(const char* icon_path);
int eui_tray_is_initialized(void);
void eui_tray_poll(int blocking);
int eui_tray_consume_show_requested(void);
int eui_tray_consume_exit_requested(void);
void eui_tray_shutdown(void);

#ifdef __cplusplus
}
#endif
