/*
 * Stub implementations of backtrace functions
 * Used when libunwind is not available
 */

#include <stdio.h>
#include <string.h>

/* Match the real backtrace.h structure */
#define BACKTRACE_FRAME_COUNT_MAX 128

struct backtrace_frame {
    void *ip;
    void *sp;
};

struct backtrace {
    int frame_count;
    struct backtrace_frame frames[BACKTRACE_FRAME_COUNT_MAX];
};

typedef struct backtrace backtrace_t;
typedef struct backtrace_frame backtrace_frame_t;

int backtrace_collect(backtrace_t *bt, void *frame __attribute__((unused)))
{
    if (bt == NULL) return -1;
    memset(bt, 0, sizeof(*bt));
    bt->frame_count = 1;  /* At least 1 frame to satisfy assertions */
    return 0;
}

int backtrace_snprint(char *buf, int size, backtrace_t *bt __attribute__((unused)))
{
    if (buf && size > 0) {
        snprintf(buf, size, "[backtrace unavailable]");
    }
    return 0;
}

int backtrace_print(backtrace_t *bt __attribute__((unused)))
{
    return 0;
}

void backtrace_lua_init(void)
{
}

int backtrace_lua_collect(void)
{
    return 0;
}

int backtrace_lua_cat(char *buf, int size)
{
    if (buf && size > 0) {
        snprintf(buf, size, "[lua backtrace unavailable]");
    }
    return 0;
}

int backtrace_lua_stack_push(void __attribute__((unused)))
{
    return 0;
}
