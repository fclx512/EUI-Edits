#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void eui_set_application_icon_rgba(int width, int height, const unsigned char* pixels);
void eui_set_native_child_window(void* parentWindow, void* childWindow, int enabled);

#ifdef __cplusplus
}
#endif
