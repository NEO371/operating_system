#include "gui.h"

#include "font.h"
#include "kernel.h"

#include <stdint.h>

static volatile uint8_t *framebuffer;
static uint32_t screen_width;
static uint32_t screen_height;
static uint32_t screen_pitch;
static uint8_t bytes_per_pixel;
static uint8_t red_position;
static uint8_t red_size;
static uint8_t green_position;
static uint8_t green_size;
static uint8_t blue_position;
static uint8_t blue_size;

#define CURSOR_WIDTH 12u
#define CURSOR_HEIGHT 16u
static uint8_t cursor_saved[CURSOR_HEIGHT][CURSOR_WIDTH][4];
static uint32_t cursor_x;
static uint32_t cursor_y;
static uint8_t cursor_visible;
static uint8_t cursor_painting;
static int32_t translation_x;
static int32_t translation_y;

static const uint16_t cursor_shape[CURSOR_HEIGHT] = {
    0x800u, 0xC00u, 0xE00u, 0xF00u, 0xF80u, 0xFC0u, 0xFE0u, 0xFF0u,
    0xFF8u, 0xFFCu, 0xFC0u, 0xDE0u, 0x8F0u, 0x070u, 0x038u, 0x018u
};

static uint32_t pack_component(uint8_t component, uint8_t position, uint8_t size) {
    uint32_t mask;

    if (size == 0u || size > 8u) {
        return 0u;
    }
    mask = (1u << size) - 1u;
    return (((uint32_t)component * mask) / 255u) << position;
}

static uint32_t pack_rgb(uint32_t color) {
    return pack_component((uint8_t)(color >> 16), red_position, red_size) |
           pack_component((uint8_t)(color >> 8), green_position, green_size) |
           pack_component((uint8_t)color, blue_position, blue_size);
}

static uint8_t blend_channel(uint8_t first, uint8_t second, uint32_t amount) {
    return (uint8_t)(((uint32_t)first * (255u - amount) +
                      (uint32_t)second * amount) / 255u);
}

static uint32_t blend_rgb(uint32_t first, uint32_t second, uint32_t amount) {
    return ((uint32_t)blend_channel((uint8_t)(first >> 16),
                                    (uint8_t)(second >> 16), amount) << 16) |
           ((uint32_t)blend_channel((uint8_t)(first >> 8),
                                    (uint8_t)(second >> 8), amount) << 8) |
           blend_channel((uint8_t)first, (uint8_t)second, amount);
}

static volatile uint8_t *pixel_address(uint32_t x, uint32_t y) {
    return framebuffer + y * screen_pitch + x * bytes_per_pixel;
}

static void raw_pixel(uint32_t x, uint32_t y, uint32_t color) {
    volatile uint8_t *destination = pixel_address(x, y);
    uint32_t packed = pack_rgb(color);
    uint8_t byte;
    for (byte = 0u; byte < bytes_per_pixel; ++byte)
        destination[byte] = (uint8_t)(packed >> (byte * 8u));
}

static void save_cursor_background(void) {
    uint32_t row, column, byte;
    for (row = 0u; row < CURSOR_HEIGHT; ++row) {
        for (column = 0u; column < CURSOR_WIDTH; ++column) {
            uint32_t x = cursor_x + column;
            uint32_t y = cursor_y + row;
            if (x >= screen_width || y >= screen_height) continue;
            for (byte = 0u; byte < bytes_per_pixel; ++byte)
                cursor_saved[row][column][byte] = pixel_address(x, y)[byte];
        }
    }
}

static void restore_cursor_background(void) {
    uint32_t row, column, byte;
    for (row = 0u; row < CURSOR_HEIGHT; ++row) {
        for (column = 0u; column < CURSOR_WIDTH; ++column) {
            uint32_t x = cursor_x + column;
            uint32_t y = cursor_y + row;
            if (x >= screen_width || y >= screen_height) continue;
            for (byte = 0u; byte < bytes_per_pixel; ++byte)
                pixel_address(x, y)[byte] = cursor_saved[row][column][byte];
        }
    }
}

static int cursor_is_edge(uint32_t row, uint32_t column) {
    uint32_t neighbor_row, neighbor_column;
    if ((cursor_shape[row] & (0x800u >> column)) == 0u) return 0;
    if (row == 0u || row + 1u == CURSOR_HEIGHT || column == 0u ||
        column + 1u == CURSOR_WIDTH) return 1;
    for (neighbor_row = row - 1u; neighbor_row <= row + 1u; ++neighbor_row) {
        for (neighbor_column = column - 1u; neighbor_column <= column + 1u;
             ++neighbor_column) {
            if ((cursor_shape[neighbor_row] & (0x800u >> neighbor_column)) == 0u)
                return 1;
        }
    }
    return 0;
}

static int cursor_pixel_color(uint32_t row, uint32_t column, uint32_t *color) {
    if ((cursor_shape[row] & (0x800u >> column)) == 0u) return 0;
    *color = cursor_is_edge(row, column) ? GUI_BLACK : GUI_WHITE;
    return 1;
}

static void paint_cursor(void) {
    uint32_t row, column, color;
    cursor_painting = 1u;
    for (row = 0u; row < CURSOR_HEIGHT; ++row) {
        for (column = 0u; column < CURSOR_WIDTH; ++column) {
            uint32_t x = cursor_x + column;
            uint32_t y = cursor_y + row;
            if (x < screen_width && y < screen_height &&
                cursor_pixel_color(row, column, &color)) raw_pixel(x, y, color);
        }
    }
    cursor_painting = 0u;
}

int gui_init(const struct multiboot_info *info) {
    /* Bit 12 signals framebuffer information; type 1 is direct RGB. */
    if ((info->flags & (1u << 12)) == 0u || info->framebuffer_type != 1u ||
        info->framebuffer_bpp < 24u || (info->framebuffer_addr >> 32) != 0u) {
        return 0;
    }

    framebuffer = (volatile uint8_t *)(uintptr_t)(uint32_t)info->framebuffer_addr;
    screen_width = info->framebuffer_width;
    screen_height = info->framebuffer_height;
    screen_pitch = info->framebuffer_pitch;
    bytes_per_pixel = (uint8_t)(info->framebuffer_bpp / 8u);
    red_position = info->red_field_position;
    red_size = info->red_mask_size;
    green_position = info->green_field_position;
    green_size = info->green_mask_size;
    blue_position = info->blue_field_position;
    blue_size = info->blue_mask_size;

    return screen_width != 0u && screen_height != 0u && bytes_per_pixel != 0u;
}

uint32_t gui_width(void) { return screen_width; }
uint32_t gui_height(void) { return screen_height; }

void gui_draw_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= screen_width || y >= screen_height) {
        return;
    }
    if (cursor_visible != 0u && cursor_painting == 0u &&
        x >= cursor_x && x - cursor_x < CURSOR_WIDTH &&
        y >= cursor_y && y - cursor_y < CURSOR_HEIGHT) {
        uint32_t column = x - cursor_x;
        uint32_t row = y - cursor_y;
        uint32_t byte;
        uint32_t cursor_color;
        volatile uint8_t *source = pixel_address(x, y);
        uint32_t packed = pack_rgb(color);
        for (byte = 0u; byte < bytes_per_pixel; ++byte) {
            cursor_saved[row][column][byte] = (uint8_t)(packed >> (byte * 8u));
            source[byte] = cursor_saved[row][column][byte];
        }
        if (cursor_pixel_color(row, column, &cursor_color))
            raw_pixel(x, y, cursor_color);
    } else {
        raw_pixel(x, y, color);
    }
}

void gui_draw_desktop_background(void) {
    uint32_t x, y;
    if (framebuffer == 0 || screen_width == 0u || screen_height == 0u) return;
    for (y = 0u; y < screen_height; ++y) {
        uint32_t vertical = screen_height > 1u ?
            y * 255u / (screen_height - 1u) : 0u;
        uint32_t left = blend_rgb(0x23194Bu, 0x123C55u, vertical);
        uint32_t right = blend_rgb(0x103A63u, 0x182344u, vertical);
        for (x = 0u; x < screen_width; ++x) {
            uint32_t horizontal = screen_width > 1u ?
                x * 255u / (screen_width - 1u) : 0u;
            gui_draw_pixel(x, y, blend_rgb(left, right, horizontal));
        }
    }
}

static uint32_t translated_coordinate(uint32_t value, int32_t offset) {
    int64_t translated = (int64_t)value + offset;
    if (translated < 0) return 0u;
    if (translated > UINT32_MAX) return UINT32_MAX;
    return (uint32_t)translated;
}

void gui_draw_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                   uint32_t color) {
    uint32_t row;
    uint32_t column;
    uint32_t end_x;
    uint32_t end_y;

    x = translated_coordinate(x, translation_x);
    y = translated_coordinate(y, translation_y);
    if (x >= screen_width || y >= screen_height) {
        return;
    }
    end_x = width > screen_width - x ? screen_width : x + width;
    end_y = height > screen_height - y ? screen_height : y + height;
    for (row = y; row < end_y; ++row) {
        for (column = x; column < end_x; ++column) {
            gui_draw_pixel(column, row, color);
        }
    }
}

void gui_draw_char(uint32_t x, uint32_t y, char character, uint32_t color) {
    uint8_t row;
    uint8_t column;
    uint8_t glyph = (uint8_t)character;

    x = translated_coordinate(x, translation_x);
    y = translated_coordinate(y, translation_y);
    if (glyph < FONT_FIRST || glyph > FONT_LAST) {
        glyph = (uint8_t)'?';
    }
    for (row = 0u; row < 8u; ++row) {
        for (column = 0u; column < 8u; ++column) {
            if ((font8x8[glyph - FONT_FIRST][row] & (0x80u >> column)) != 0u) {
                gui_draw_pixel(x + column, y + row, color);
            }
        }
    }
}

void gui_draw_string(uint32_t x, uint32_t y, const char *string, uint32_t color) {
    while (*string != '\0') {
        gui_draw_char(x, y, *string, color);
        x += 8u;
        ++string;
    }
}

void gui_draw_text_box(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                       uint32_t line_height, const char *text, uint32_t color) {
    uint32_t columns = width / 8u;
    uint32_t rows;
    uint32_t column = 0u;
    uint32_t row = 0u;
    if (text == 0 || columns == 0u || line_height < 8u || height < 8u) return;
    rows = height / line_height;
    if (rows == 0u) rows = 1u;
    while (*text != '\0' && row < rows) {
        uint32_t word_length = 0u;
        const char *word;
        if (*text == '\r') { ++text; continue; }
        if (*text == '\n') {
            ++text;
            ++row;
            column = 0u;
            continue;
        }
        if (*text == ' ' || *text == '\t') {
            if (column != 0u) {
                if (column == columns) {
                    ++row;
                    column = 0u;
                    if (row >= rows) break;
                } else {
                    gui_draw_char(x + column * 8u,
                                  y + row * line_height, ' ', color);
                    ++column;
                }
            }
            do { ++text; } while (*text == ' ' || *text == '\t');
            continue;
        }
        word = text;
        while (text[word_length] != '\0' && text[word_length] != '\n' &&
               text[word_length] != '\r' && text[word_length] != ' ' &&
               text[word_length] != '\t') ++word_length;
        if (column != 0u && word_length <= columns &&
            column + word_length > columns) {
            ++row;
            column = 0u;
            if (row >= rows) break;
        }
        while (word_length != 0u && row < rows) {
            gui_draw_char(x + column * 8u, y + row * line_height,
                          *word++, color);
            ++column;
            --word_length;
            if (column == columns && word_length != 0u) {
                ++row;
                column = 0u;
            }
        }
        text = word;
        while (*text != '\0' && *text != '\n' && *text != '\r' &&
               *text != ' ' && *text != '\t') ++text;
    }
}

void gui_draw_window(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                     const char *title) {
    uint32_t close_x;

    if (width < 24u || height < 42u) {
        return;
    }
    gui_draw_rect(x + 5u, y + 6u, width, height, GUI_SHADOW);
    gui_draw_rect(x, y, width, height, GUI_BORDER);
    gui_draw_rect(x + 2u, y + 2u, width - 4u, 26u, GUI_TITLE);
    gui_draw_rect(x + 2u, y + 26u, width - 4u, 2u, GUI_GREEN);
    gui_draw_rect(x + 2u, y + 28u, width - 4u, height - 30u, GUI_CLIENT);
    gui_draw_string(x + 8u, y + 10u, title, GUI_WHITE);

    close_x = x + width - 22u;
    gui_draw_rect(close_x, y + 6u, 14u, 14u, GUI_RED);
    gui_draw_char(close_x + 3u, y + 9u, 'X', GUI_WHITE);
}

void gui_cursor_hide(void) {
    if (cursor_visible == 0u) return;
    restore_cursor_background();
    cursor_visible = 0u;
}

void gui_cursor_show(void) {
    if (framebuffer == 0 || screen_width == 0u || screen_height == 0u) return;
    if (cursor_visible != 0u) return;
    save_cursor_background();
    cursor_visible = 1u;
    paint_cursor();
}

void gui_cursor_move(uint32_t x, uint32_t y) {
    if (framebuffer == 0 || screen_width == 0u || screen_height == 0u) return;
    if (x >= screen_width) x = screen_width - 1u;
    if (y >= screen_height) y = screen_height - 1u;
    /* Button-only mouse packets should not erase and repaint a stationary cursor. */
    if (cursor_visible != 0u && x == cursor_x && y == cursor_y) return;
    gui_cursor_hide();
    cursor_x = x;
    cursor_y = y;
    gui_cursor_show();
}

void gui_set_translation(int32_t x, int32_t y) {
    translation_x = x;
    translation_y = y;
}
