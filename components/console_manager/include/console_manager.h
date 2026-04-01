#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONSOLE_MANAGER_MAX_LINES      48
#define CONSOLE_MANAGER_MAX_LINE_LEN   160

typedef struct {
    uint32_t seq;
    char     text[CONSOLE_MANAGER_MAX_LINE_LEN];
} console_line_t;

void console_manager_init(void);

size_t console_manager_get_since(uint32_t *cursor,
                                 console_line_t *out,
                                 size_t max_count,
                                 bool *dropped);
