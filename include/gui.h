#ifndef MINI_OS_GUI_H
#define MINI_OS_GUI_H

#include <stdint.h>

struct multiboot_info;

enum {
    GUI_BLACK       = 0x000000u,
    GUI_WHITE       = 0xFFFFFFu,
    GUI_DESKTOP     = 0x23194Bu,
    GUI_TASKBAR     = 0x10162Eu,
    GUI_TITLE       = 0x332862u,
    GUI_WINDOW      = 0x211C3Fu,
    GUI_CLIENT      = 0x171B34u,
    GUI_BORDER      = 0x4B4C86u,
    GUI_SHADOW      = 0x080B1Cu,
    GUI_TEXT        = 0xF4F0FFu,
    GUI_MUTED       = 0xB3B6D8u,
    GUI_GREEN       = 0x50E3C2u,
    GUI_YELLOW      = 0xFFD166u,
    GUI_RED         = 0xFF6B81u,
    GUI_BLUE        = 0x62B8FFu,
    GUI_PURPLE      = 0xB279FFu
};

int gui_init(const struct multiboot_info *info);
uint32_t gui_width(void);
uint32_t gui_height(void);
void gui_draw_pixel(uint32_t x, uint32_t y, uint32_t color);
void gui_draw_desktop_background(void);
void gui_draw_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                   uint32_t color);
void gui_draw_char(uint32_t x, uint32_t y, char character, uint32_t color);
void gui_draw_string(uint32_t x, uint32_t y, const char *string, uint32_t color);
void gui_draw_text_box(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                       uint32_t line_height, const char *text, uint32_t color);
void gui_draw_window(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                     const char *title);
void gui_cursor_move(uint32_t x, uint32_t y);
void gui_cursor_hide(void);
void gui_cursor_show(void);
void gui_set_translation(int32_t x, int32_t y);

#endif
