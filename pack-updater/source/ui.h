#pragma once

#include <stdint.h>

int ui_init(void);
void ui_exit(void);

void ui_begin(uint32_t bg);
void ui_text(int x, int y, float size, uint32_t rgba, const char *utf8);
void ui_end(void);
void ui_idle(void);

static inline uint32_t ui_rgba(unsigned r, unsigned g, unsigned b, unsigned a)
{
    return (r & 0xffu) | ((g & 0xffu) << 8) | ((b & 0xffu) << 16) | ((a & 0xffu) << 24);
}
