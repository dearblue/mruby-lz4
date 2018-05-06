#include <string.h>
#include <stdlib.h>
#include <micro-co.h>
#include "unlz4-gradual.h"

//#define NO_BUILTIN_EXPECT

#define HAVE_BUILTIN_EXPECT

#if defined(NO_BUILTIN_EXPECT)
# undef HAVE_BUILTIN_EXPECT
#else
# if !defined(HAVE_BUILTIN_EXPECT) && (defined(__GNUC__) || defined(__clang__))
#  define HAVE_BUILTIN_EXPECT 1
# endif
#endif

#if defined(HAVE_BUILTIN_EXPECT)
# define likely(x)      __builtin_expect(!!(x), 1)
# define unlikely(x)    __builtin_expect(!!(x), 0)
#else
# define likely(x)      (x)
# define unlikely(x)    (x)
#endif

static int32_t
loadu16le(const void *ptr)
{
    const uint8_t *p = (const uint8_t *)ptr;

    return (((uint16_t)p[0]) << 0) |
           (((uint16_t)p[1]) << 8);
}

#define MINIMAL_MATCH_LENGTH 4

struct unlz4_gradual_real
{
    struct unlz4_gradual port;

    co_state_t co_state;

//    int32_t expand255; /* NOTE: リテラル長やコピー長を広げる時に使われる。 */
    int32_t literal_length;
    int32_t match_length;
    int32_t offset;
    int32_t prefix_length;
    int32_t prefix_capacity;
    char prefix[];
};

static void
get_ready_to_suspend(struct unlz4_gradual_real *p, const char *const begin_in, const char *const begin_out)
{
    size_t used_in = p->port.next_in - begin_in;
    size_t used_out = p->port.next_out - begin_out;

    if (p->prefix_capacity <= used_out) {
        memcpy(p->prefix, p->port.next_out - p->prefix_capacity, p->prefix_capacity);
        p->prefix_length = p->prefix_capacity;
    } else {
        int32_t cutlen = used_out - (p->prefix_capacity - p->prefix_length);

        if (cutlen <= 0) {
            memcpy(p->prefix + p->prefix_length, begin_out, used_out);
            p->prefix_length += used_out;
        } else {
            memmove(p->prefix, p->prefix + cutlen, p->prefix_length - cutlen);
            memcpy(p->prefix + p->prefix_capacity - used_out, begin_out, used_out);

            p->prefix_length = p->prefix_capacity;
        }
    }

    p->port.avail_in -= used_in;
    p->port.total_in += used_in;
    p->port.avail_out -= used_out;
    p->port.total_out += used_out;
}

static enum unlz4_gradual_status
expand_length(struct unlz4_gradual_real *p, int32_t *len, const uintptr_t term_in)
{
#if 0
    enum { slice_size = sizeof(int32_t) };

    uint32_t slice = (term_in - (uintptr_t)p->port.next_in) / slice_size;

    for (; slice > 0; slice --) {
        /* NOTE: バイトオーダーに関わる問題はないが、8 bit per bytes (octet) に限定される */

        if (loadi32(p->port.next_in) != -1) { break; }

        //p->expand255 += slice_size;
        p->port.next_in += 255 * slice_size;
    }
#endif
    while (likely((term_in > (uintptr_t)p->port.next_in))) {
        uint8_t w = *p->port.next_in ++;

        *len += w;

        if (w != 255) { return UNLZ4_GRADUAL_OK; }
    }

    return UNLZ4_GRADUAL_NEED_INPUT;
}

enum { copy_word_size = 16, copy_word_size_mask = copy_word_size - 1 };

static void
fast_copy(char *dest, const char *src, int32_t len)
{
#ifdef UNLZ4_GRADUAL_USE_MEMCPY
    for (; len >= copy_word_size; len -= copy_word_size) {
        memcpy(dest, src, copy_word_size);
        dest += copy_word_size;
        src += copy_word_size;
    }

    memcpy(dest, src, len);
#else
    for (; len > 0; len --) { *dest ++ = *src ++; }
#endif
}

static void
copy(char *dest, const char *src, int32_t len)
{
#ifdef UNLZ4_GRADUAL_USE_MEMCPY
    size_t s;

    if ((s = dest - src) < len) {
        if (s < copy_word_size) {
            s = copy_word_size - s;
            len -= s;

            for (; s > 0; s --) {
                *dest ++ = *src ++;
            }
        }

        if (len < copy_word_size) {
            for (; len > 0; len --) {
                *dest ++ = *src ++;
            }

            return;
        }
    }
#endif

    fast_copy(dest, src, len);
}

static enum unlz4_gradual_status
copy_literal(struct unlz4_gradual_real *p, int32_t len, const uintptr_t term_in, const uintptr_t term_out)
{
    enum unlz4_gradual_status s = UNLZ4_GRADUAL_OK;

    if (unlikely(len > (term_in - (uintptr_t)p->port.next_in))) {
        len = term_in - (uintptr_t)p->port.next_in;
        s = UNLZ4_GRADUAL_NEED_INPUT;
    }

    if (unlikely(len > (term_out - (uintptr_t)p->port.next_out))) {
        len = term_out - (uintptr_t)p->port.next_out;
        s = UNLZ4_GRADUAL_NEED_OUTPUT;
    }

    fast_copy(p->port.next_out, p->port.next_in, len);

    p->port.next_in += len;
    p->port.next_out += len;
    p->literal_length -= len;

    return s;
}

static enum unlz4_gradual_status
copy_match(struct unlz4_gradual_real *p, const char *src, int32_t len, const uintptr_t term_out)
{
    enum unlz4_gradual_status s;

    if (unlikely(len > (term_out - (uintptr_t)p->port.next_out))) {
        len = term_out - (uintptr_t)p->port.next_out;
        s = UNLZ4_GRADUAL_NEED_OUTPUT;
    } else {
        s = UNLZ4_GRADUAL_OK;
    }

    copy(p->port.next_out, src, len);
    p->port.next_out += len;
    p->match_length -= len;

    return s;
}

enum unlz4_gradual_status
unlz4_gradual(struct unlz4_gradual *g)
{
    struct unlz4_gradual_real *p = (struct unlz4_gradual_real *)g;
    const char *const begin_in = p->port.next_in;
    const uintptr_t term_in = (uintptr_t)begin_in + p->port.avail_in;
    const char *const begin_out = p->port.next_out;
    const uintptr_t term_out = (uintptr_t)begin_out + p->port.avail_out;

    co_begin(&p->co_state);

    for (;;) {
        {
            /* 最初のトークンを読み込む */

            while (unlikely((term_in - (uintptr_t)p->port.next_in) < 1)) {
                get_ready_to_suspend(p, begin_in, begin_out);
                co_yield(UNLZ4_GRADUAL_NEED_INPUT);
            }

            {
                uint8_t w = *p->port.next_in ++;

                p->literal_length = (w >> 4) & 0x0f;
                p->match_length = (w >> 0) & 0x0f;
            }
        }

        {
            /* リテラル長を読み込む */

            if (p->literal_length == 15) {
                //p->expand255 = 0;

                while (unlikely(expand_length(p, &p->literal_length, term_in) != UNLZ4_GRADUAL_OK)) {
                    get_ready_to_suspend(p, begin_in, begin_out);
                    co_yield(UNLZ4_GRADUAL_NEED_INPUT);
                }
            }
        }

        {
            /* リテラルデータのコピー */

            if (p->literal_length > 0) {
                enum unlz4_gradual_status s;

                while (unlikely((s = copy_literal(p, p->literal_length, term_in, term_out)) != UNLZ4_GRADUAL_OK)) {
                    get_ready_to_suspend(p, begin_in, begin_out);
                    co_yield(s);
                }
            }
        }

        {
            /* 一致範囲の位置を読み込む */

            if (likely(term_in - (uintptr_t)p->port.next_in >= 2)) {
                p->offset = loadu16le(p->port.next_in);
                p->port.next_in += 2;
            } else {
                while (unlikely((term_in - (uintptr_t)p->port.next_in) < 1)) {
                    get_ready_to_suspend(p, begin_in, begin_out);
                    co_yield(p->match_length == 0 ?
                                UNLZ4_GRADUAL_MAYBE_FINISHED :
                                UNLZ4_GRADUAL_NEED_INPUT);
                }

                p->offset = *(const uint8_t *)p->port.next_in ++;

                while (unlikely((term_in - (uintptr_t)p->port.next_in) < 1)) {
                    get_ready_to_suspend(p, begin_in, begin_out);
                    co_yield(UNLZ4_GRADUAL_NEED_INPUT);
                }

                p->offset |= (uint16_t)(*(const uint8_t *)p->port.next_in ++) << 8;
            }
        }

        {
            /* 一致長を読み込む */

            if (p->match_length == 15) {
                //p->expand255 = 0;

                while (unlikely(expand_length(p, &p->match_length, term_in) != UNLZ4_GRADUAL_OK)) {
                    get_ready_to_suspend(p, begin_in, begin_out);
                    co_yield(UNLZ4_GRADUAL_NEED_INPUT);
                }
            }

            p->match_length += MINIMAL_MATCH_LENGTH;
        }

        {
            /* 一致範囲のコピー */

            int32_t backward;

            for (;;) {
                backward = p->offset - (p->port.next_out - begin_out);

                if (backward <= 0) {
                    if (likely(copy_match(p, p->port.next_out - p->offset, p->match_length, term_out) == UNLZ4_GRADUAL_OK)) {
                        break;
                    }
                } else if (unlikely(backward > p->prefix_length)) {
                    get_ready_to_suspend(p, begin_in, begin_out);
                    co_halt(UNLZ4_GRADUAL_ERROR_OUT_OF_PREFIX_BUFFER);
                } else if (backward > p->match_length) {
                    if (likely(copy_match(p, p->prefix + p->prefix_length - backward, p->match_length, term_out) == UNLZ4_GRADUAL_OK)) {
                        break;
                    }
                } else {
                    if (likely(copy_match(p, p->prefix + p->prefix_length - backward, backward, term_out) == UNLZ4_GRADUAL_OK &&
                               copy_match(p, begin_out, p->match_length, term_out) == UNLZ4_GRADUAL_OK)) {
                        break;
                    }
                }

                get_ready_to_suspend(p, begin_in, begin_out);
                co_yield(UNLZ4_GRADUAL_NEED_OUTPUT);
            }
        }
    }

    co_end();

    return UNLZ4_GRADUAL_ERROR_UNEXPECT_REACHED_HERE;
}

enum unlz4_gradual_status
unlz4_gradual_alloc(struct unlz4_gradual **g, int32_t prefix_capacity, void *alloc(void *user, size_t), void *user)
{
    if ((uint32_t)prefix_capacity > UNLZ4_GRADUAL_MAX_PREFIX_LENGTH) {
        prefix_capacity = UNLZ4_GRADUAL_MAX_PREFIX_LENGTH;
    }

    struct unlz4_gradual_real *p;
    int allocsize = sizeof(struct unlz4_gradual_real) + prefix_capacity;

    if (alloc) {
        p = (struct unlz4_gradual_real *)alloc(user, allocsize);
    } else {
#ifdef UNLZ4_GRADUAL_NO_MALLOC
        return UNLZ4_GRADUAL_ERROR_NO_MEMORY;
#else
        p = (struct unlz4_gradual_real *)malloc(allocsize);
#endif
    }

    if (!p) { return UNLZ4_GRADUAL_ERROR_NO_MEMORY; }

    memset(p, 0, sizeof(struct unlz4_gradual_real));
    p->co_state = CO_INIT;
    p->prefix_capacity = prefix_capacity;

    *g = (struct unlz4_gradual *)p;

    return UNLZ4_GRADUAL_OK;
}

enum unlz4_gradual_status
unlz4_gradual_reset(struct unlz4_gradual *g, const void *prefix, int32_t prefixlen)
{
    struct unlz4_gradual_real *p = (struct unlz4_gradual_real *)g;

    if (prefixlen > UNLZ4_GRADUAL_MAX_PREFIX_LENGTH) {
        prefix = (const char *)prefix + prefixlen - UNLZ4_GRADUAL_MAX_PREFIX_LENGTH;
        prefixlen = UNLZ4_GRADUAL_MAX_PREFIX_LENGTH;
    }

    if (prefixlen > p->prefix_capacity) {
        return UNLZ4_GRADUAL_ERROR_OUT_OF_PREFIX_BUFFER;
    }

    if (prefix == NULL && prefixlen < 0) {
        /* NOTE: keep prefix buffer contents */
    } else if (prefix == NULL || prefixlen < 1) {
        p->prefix_length = 0;
    } else {
        memcpy(p->prefix, prefix, prefixlen);
        p->prefix_length = prefixlen;
    }

    int32_t preflen = p->prefix_length;
    int32_t prefcapa = p->prefix_capacity;
    memset(&p->co_state, 0, sizeof(struct unlz4_gradual_real) - sizeof(struct unlz4_gradual));
    p->co_state = CO_INIT;
    p->prefix_length = preflen;
    p->prefix_capacity = prefcapa;

    return UNLZ4_GRADUAL_OK;
}

const char *
unlz4_gradual_str_status(enum unlz4_gradual_status s)
{
    switch (s) {
    case UNLZ4_GRADUAL_OK:
        return "OK";
    case UNLZ4_GRADUAL_MAYBE_FINISHED:
        return "MAYBE_FINISHED";
    case UNLZ4_GRADUAL_NEED_INPUT:
        return "NEED_INPUT";
    case UNLZ4_GRADUAL_NEED_OUTPUT:
        return "NEED_OUTPUT";
    case UNLZ4_GRADUAL_ERROR_NO_MEMORY:
        return "ERROR_NO_MEMORY";
    case UNLZ4_GRADUAL_ERROR_OUT_OF_PREFIX_BUFFER:
        return "ERROR_OUT_OF_PREFIX_BUFFER";
    case UNLZ4_GRADUAL_ERROR_UNEXPECT_REACHED_HERE:
        return "ERROR_UNEXPECT_REACHED_HERE";
    default:
        return "unknown";
    }
}
