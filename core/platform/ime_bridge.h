#pragma once

struct GLFWwindow;

#ifdef __cplusplus
extern "C" {
#endif

void eui_ime_set_cursor_rect(struct GLFWwindow* window, double x, double y, double width, double height);
void eui_ime_set_cursor_rect_with_font(struct GLFWwindow* window, double x, double y, double width, double height, double fontHeight);
void eui_ime_install_message_filter(struct GLFWwindow* window);
void eui_ime_uninstall_message_filter(struct GLFWwindow* window);
int eui_ime_is_composing(struct GLFWwindow* window);
int eui_ime_get_composition_string_utf8(struct GLFWwindow* window, char* buffer, int bufferSize);
void eui_ime_clear_composition(struct GLFWwindow* window);

#ifdef __cplusplus
}
#endif
