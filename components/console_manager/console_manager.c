#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_log.h"
#include "console_manager.h"

typedef int (*vprintf_like_t)(const char *fmt, va_list args);

static console_line_t s_lines[CONSOLE_MANAGER_MAX_LINES];
static size_t         s_head = 0;
static size_t         s_count = 0;
static uint32_t       s_next_seq = 1;
static char           s_partial_line[CONSOLE_MANAGER_MAX_LINE_LEN];
static size_t         s_partial_len = 0;
static bool           s_initialized = false;
static vprintf_like_t s_prev_vprintf = NULL;
static portMUX_TYPE   s_lock = portMUX_INITIALIZER_UNLOCKED;

static void store_line_locked(const char *line)
{
    if (!line || !line[0]) return;

    console_line_t *slot = &s_lines[s_head];
    slot->seq = s_next_seq++;
    strlcpy(slot->text, line, sizeof(slot->text));
    s_head = (s_head + 1) % CONSOLE_MANAGER_MAX_LINES;
    if (s_count < CONSOLE_MANAGER_MAX_LINES) s_count++;
}

static void store_text_lines(const char *text)
{
    if (!text || !text[0]) return;

    portENTER_CRITICAL(&s_lock);

    for (const char *p = text; *p; p++) {
        char ch = *p;
        if (ch == '\r') continue;

        if (ch == '\n') {
            if (s_partial_len > 0) {
                s_partial_line[s_partial_len] = '\0';
                store_line_locked(s_partial_line);
                s_partial_len = 0;
                s_partial_line[0] = '\0';
            }
            continue;
        }

        if (s_partial_len >= sizeof(s_partial_line) - 1) {
            s_partial_line[s_partial_len] = '\0';
            store_line_locked(s_partial_line);
            s_partial_len = 0;
            s_partial_line[0] = '\0';
        }

        s_partial_line[s_partial_len++] = ch;
        s_partial_line[s_partial_len] = '\0';
    }

    portEXIT_CRITICAL(&s_lock);
}

static int console_vprintf(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int written = s_prev_vprintf ? s_prev_vprintf(fmt, args) : vprintf(fmt, args);

    char buf[256];
    int needed = vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);

    if (needed > 0) {
        if ((size_t)needed < sizeof(buf)) {
            store_text_lines(buf);
        } else {
            buf[sizeof(buf) - 5] = '.';
            buf[sizeof(buf) - 4] = '.';
            buf[sizeof(buf) - 3] = '.';
            buf[sizeof(buf) - 2] = '\0';
            store_text_lines(buf);
        }
    }

    return written;
}

void console_manager_init(void)
{
    if (s_initialized) return;
    s_prev_vprintf = esp_log_set_vprintf(console_vprintf);
    s_initialized = true;
}

size_t console_manager_get_since(uint32_t *cursor,
                                 console_line_t *out,
                                 size_t max_count,
                                 bool *dropped)
{
    if (!cursor || !out || max_count == 0) return 0;

    portENTER_CRITICAL(&s_lock);

    uint32_t oldest_seq = s_next_seq - (uint32_t)s_count;
    uint32_t seq = (*cursor == 0 || *cursor < oldest_seq) ? oldest_seq : *cursor;
    bool lost = (*cursor != 0 && *cursor < oldest_seq);
    size_t produced = 0;

    while (seq < s_next_seq && produced < max_count) {
        size_t offset = (size_t)(seq - oldest_seq);
        size_t idx = (s_head + CONSOLE_MANAGER_MAX_LINES - s_count + offset) % CONSOLE_MANAGER_MAX_LINES;
        out[produced++] = s_lines[idx];
        seq++;
    }

    *cursor = seq;
    if (dropped) *dropped = lost;

    portEXIT_CRITICAL(&s_lock);
    return produced;
}
