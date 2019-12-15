#include <mruby.h>
#include <mruby/class.h>
#include <mruby/hash.h>
#include <mruby/string.h>
#include <mruby/value.h>
#include <mruby/data.h>
#include <mruby/variable.h>
#include <mruby/error.h>
#include <lz4.h>
#include <lz4hc.h>
#include <lz4frame.h>
#include <mruby-aux.h>
#include <mruby-aux/scanhash.h>
#include <mruby-aux/string.h>
#include <mruby-aux/fakedin.h>
#include <string.h>
#include <sys/types.h> /* for ssize_t */

#define LOGF(FORMAT, ...) do { fprintf(stderr, "%s:%d:%s: " FORMAT "\n", __FILE__, __LINE__, __func__, __VA_ARGS__); } while (0)

#define AUX_LZ4_DEFAULT_PARTIAL_SIZE ((uint32_t)256 << 10)

#ifdef MRB_INT16
#   define AUX_LZ4_PREFIX_MAX_CAPACITY  MRBX_STR_MAX
#else
#   define AUX_LZ4_PREFIX_MAX_CAPACITY  (65536L)
#endif

#define AUX_OR_DEFAULT(primary, secondary) (NIL_P(primary) ? (secondary) : (primary))
#define CLAMP(n, min, max) (n < min ? min : (n > max ? max : n))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define AUX_STR_MAX MRBX_STR_MAX

#define AUX_NOT_REACHED_HERE                                \
    do {                                                    \
        mrb_bug(mrb, "MUST NOT REACHED HERE - %S:%S:%S",    \
                mrb_str_new_lit(mrb, __FILE__),             \
                mrb_fixnum_value(__LINE__),                 \
                mrb_str_new_cstr(mrb, __func__));           \
    } while (0)                                             \


#define id_initialize mrb_intern_lit(mrb, "initialize")
#define id_read mrb_intern_lit(mrb, "read")
#define id_ivar_inport mrb_intern_lit(mrb, "inport@mruby-lz4")

/*
 * mrb_str_cat は capa 目いっぱいまで埋めると倍々に拡張するので、その代わりの関数。
 *
 * capa 目一杯まで埋めても capa を維持する。
 */
static struct RString *
aux_str_cat(MRB, struct RString *str, const char *buf, size_t len)
{
    mrb_int off = RSTR_LEN(str);
    mrbx_str_reserve(mrb, str, off + len);
    memcpy(RSTR_PTR(str) + off, buf, len);
    RSTR_SET_LEN(str, off + len);

    return str;
}

static inline VALUE
aux_str_buf_new(MRB, size_t size)
{
    return mrb_str_buf_new(mrb, MIN(size, AUX_STR_MAX));
}

static inline VALUE
aux_str_alloc(MRB, VALUE str, size_t size)
{
    if (size > AUX_STR_MAX) {
        mrb_raise(mrb, E_RUNTIME_ERROR,
                  "limitation memory allocate by mruby with build ``MRB_INT16''");
    }
    if (NIL_P(str) || MRB_FROZEN_P(RSTRING(str))) {
        str = aux_str_buf_new(mrb, size);
    } else {
        mrb_str_modify(mrb, RSTRING(str));
    }
    mrbx_str_reserve(mrb, str, size);

    return str;
}

static mrb_int
convert_to_lz4_level(MRB, VALUE level)
{
    if (NIL_P(level)) {
        return -1;
    } else {
        return mrb_int(mrb, level);
    }
}

static mrb_int
convert_to_prefix_capacity(MRB, VALUE capa)
{
    if (NIL_P(capa)) {
        return AUX_LZ4_PREFIX_MAX_CAPACITY;
    } else {
        return CLAMP(mrb_int(mrb, capa), 0, AUX_LZ4_PREFIX_MAX_CAPACITY);
    }
}

static LZ4F_blockSizeID_t
aux_lz4f_blocksizeid(MRB, VALUE size)
{
    size_t n;
    if (NIL_P(size)) {
        return LZ4F_default;
    } else if (mrb_float_p(size)) {
        n = mrb_float(size);
    } else {
        n = mrb_int(mrb, size);
    }

    if (n == 0) {
        return LZ4F_default;
    } else if (n <= (64 << 10)) {
        return LZ4F_max64KB;
    } else if (n <= (256 << 10)) {
        return LZ4F_max256KB;
    } else if (n <= (1 << 20)) {
        return LZ4F_max1MB;
    } else {
        return LZ4F_max4MB;
    }
}

static mrb_int
aux_lz4f_compression_level(MRB, VALUE level)
{
    if (NIL_P(level)) {
        return 0;
    } else {
        return mrb_int(mrb, level);
    }
}

static uint32_t
aux_to_u32(MRB, VALUE size)
{
    if (NIL_P(size)) {
        return 0;
    }

    if (mrb_float_p(size)) {
        mrb_float n = mrb_float(size);
        return CLAMP(n, 0, UINT32_MAX);
    } else {
        mrb_int n = mrb_int(mrb, size);
        return CLAMP(n, 0, UINT32_MAX);
    }
}

static uint64_t
aux_to_u64(MRB, VALUE size)
{
    if (NIL_P(size)) {
        return 0;
    }

    if (mrb_float_p(size)) {
        mrb_float n = mrb_float(size);
        return CLAMP(n, 0, UINT64_MAX);
    } else {
        mrb_int n = mrb_int(mrb, size);
        return CLAMP(n, 0, UINT64_MAX);
    }
}

static size_t
aux_to_sizet(MRB, VALUE size)
{
    if (NIL_P(size)) {
        return 0;
    }

    if (mrb_float_p(size)) {
        return mrb_float(size);
    } else {
        return mrb_int(mrb, size);
    }
}

static void
aux_lz4f_check_error(MRB, size_t status, const char *mesg)
{
    if (!LZ4F_isError(status)) { return; }

    mrb_gc_arena_restore(mrb, 0);
    if (mesg) {
        mrb_raisef(mrb, E_RUNTIME_ERROR,
                   "%S failed - %S (code:%S)",
                   mrb_str_new_cstr(mrb, mesg),
                   mrb_str_new_cstr(mrb, LZ4F_getErrorName(status)),
                   mrb_fixnum_value(-status));
    } else {
        mrb_raisef(mrb, E_RUNTIME_ERROR,
                   "lz4 frame failed - %S (code:%S)",
                   mrb_str_new_cstr(mrb, LZ4F_getErrorName(status)),
                   mrb_fixnum_value(-status));
    }
}

static const uint8_t *
expand_size(MRB, const uint8_t *p, uintptr_t const pp, int32_t *size)
{
    for (; (uintptr_t)p < pp; p ++) {
        *size += *p;
        if (*size > LZ4_MAX_INPUT_SIZE) {
            break;
        }
        if (*p < 255) {
            return p + 1;
        }
    }

    mrb_raise(mrb, E_RUNTIME_ERROR, "invalid sequence");

    return 0;
}

static int32_t
aux_lz4_scan_size(MRB, const void *p, size_t len)
{
    const uint8_t *q = (const uint8_t *)p;
    uintptr_t const qq = (uintptr_t)q + len;
    int32_t size = 0;

    while ((uintptr_t)q < qq) {
        int32_t litlen = (*q >> 4) & 0x0f;
        int32_t duplen = (*q >> 0) & 0x0f;
        q ++;

        if (litlen == 15) {
            q = expand_size(mrb, q, qq, &litlen);
        }
        size += litlen;
        if (size < litlen) {
            break;
        }
        q += litlen;
        if ((size_t)q < litlen) {
            /* buffer rounded */
            break;
        }
        if (duplen == 0 && (uintptr_t)q == qq) {
            return size;
        }

        if ((uintptr_t)q >= qq) {
            /* buffer over flow */
            break;
        }

        if (qq - (uintptr_t)q < 2) {
            /* buffer over flow */
            break;
        }

        /* unpack link offset (2 bytes) */
        q += 2;

        if (duplen == 15) {
            q = expand_size(mrb, q, qq, &duplen);
        }
        duplen += 4;
        size += duplen;
        if (size < duplen) {
            break;
        }
    }

    mrb_raise(mrb, E_RUNTIME_ERROR, "invalid sequence");

    return 0;
}

#if !defined(MRB_INT16) || !defined(WITHOUT_UNLZ4_GRADUAL)
static void
common_read_args(MRB, intptr_t *size, struct RString **dest)
{
    VALUE asize = Qnil;
    VALUE destv;
    switch (mrb_get_args(mrb, "|oS!", &asize, &destv)) {
    case 0:
        *size = -1;
        *dest = NULL;
        break;
    case 1:
        *size = (NIL_P(asize) ? -1 : mrb_int(mrb, asize));
        *dest = NULL;
        break;
    case 2:
        *size = (NIL_P(asize) ? -1 : mrb_int(mrb, asize));
        *dest = mrbx_str_ptr(mrb, destv);
        break;
    default:
        AUX_NOT_REACHED_HERE;
    }

    *dest = mrbx_str_force_recycle(mrb, *dest, (*size < 0 ? AUX_LZ4_DEFAULT_PARTIAL_SIZE : *size));
    mrbx_str_set_len(mrb, *dest, 0);
}
#endif

#ifndef MRB_INT16

/*
 * class LZ4::Encoder
 */

static LZ4F_preferences_t
aux_lz4f_encode_args(MRB, VALUE opts)
{
    VALUE level, blocksize, blocklink, checksum, size;
    MRBX_SCANHASH(mrb, opts, Qnil,
            MRBX_SCANHASH_ARGS("level", &level, Qnil),
            MRBX_SCANHASH_ARGS("blocksize", &blocksize, Qnil),
            MRBX_SCANHASH_ARGS("blocklink", &blocklink, Qtrue),
            MRBX_SCANHASH_ARGS("checksum", &checksum, Qfalse),
            MRBX_SCANHASH_ARGS("size", &size, Qnil));

    LZ4F_preferences_t prefs = {
        .frameInfo.blockSizeID = aux_lz4f_blocksizeid(mrb, size),
        .frameInfo.blockMode = (NIL_P(blocklink) || mrb_bool(blocklink)) ? LZ4F_blockLinked : LZ4F_blockIndependent,
        .frameInfo.contentChecksumFlag = (NIL_P(checksum) || mrb_bool(checksum)) ? LZ4F_contentChecksumEnabled : LZ4F_noContentChecksum,
        .frameInfo.frameType = LZ4F_frame,
        .frameInfo.contentSize = aux_to_u64(mrb, size),
        .compressionLevel = aux_lz4f_compression_level(mrb, level),
        .autoFlush = 1,
    };

    return prefs;
}

static void
enc_s_encode_args(MRB, VALUE *src, VALUE *dest, LZ4F_preferences_t *prefs)
{
    mrb_int argc;
    VALUE *argv;
    mrb_get_args(mrb, "*", &argv, &argc);
    mrb_int argc0 = argc;
    if (argc > 0 && mrb_hash_p(argv[argc - 1])) {
        *prefs = aux_lz4f_encode_args(mrb, argv[argc - 1]);
        argc --;
    } else {
        memset(prefs, 0, sizeof(*prefs));
    }

    size_t maxsize;
    switch (argc) {
    case 1:
        *src = argv[0];
        maxsize = -1;
        *dest = Qnil;
        break;
    case 2:
        *src = argv[0];
        if (mrb_string_p(argv[1])) {
            *dest = argv[1];
            maxsize = -1;
        } else {
            *dest = Qnil;
            maxsize = (NIL_P(argv[1]) ? -1 : aux_to_u64(mrb, argv[1]));
        }
        break;
    case 3:
        *src = argv[0];
        maxsize = (NIL_P(argv[1]) ? -1 : aux_to_u64(mrb, argv[1]));
        *dest = argv[2];
        break;
    default:
        mrb_raisef(mrb, E_ARGUMENT_ERROR,
                "wrong number of arguments (given %S, expect 1..3 + keywords)",
                mrb_fixnum_value(argc0));
    }

    mrb_check_type(mrb, *src, MRB_TT_STRING);

    if (maxsize == -1) {
        maxsize = LZ4F_compressFrameBound(RSTRING_LEN(*src), prefs);
    }

    maxsize = MIN(maxsize, AUX_STR_MAX);

    if (NIL_P(*dest)) {
        *dest = aux_str_buf_new(mrb, maxsize);
    } else {
        mrb_check_type(mrb, *dest, MRB_TT_STRING);
        mrbx_str_reserve(mrb, *dest, maxsize);
        mrbx_str_set_len(mrb, *dest, 0);
    }
}

/*
 * call-seq:
 *  encode(src, maxsize = nil, destbuf = "", prefs = {})
 *  encode(src, destbuf, prefs = {})
 */
static VALUE
enc_s_encode(MRB, VALUE self)
{
    VALUE src, dest;
    LZ4F_preferences_t prefs;
    enc_s_encode_args(mrb, &src, &dest, &prefs);

    size_t s = LZ4F_compressFrame(RSTRING_PTR(dest), RSTRING_CAPA(dest),
                                  RSTRING_PTR(src), RSTRING_LEN(src),
                                  &prefs);
    aux_lz4f_check_error(mrb, s, "LZ4F_compressFrame");
    mrbx_str_set_len(mrb, dest, s);
    return dest;
}

struct encoder
{
    LZ4F_cctx *lz4f;
    LZ4F_preferences_t prefs;
    VALUE io;
    VALUE outbuf;
    size_t outbufsize;
};

static void
encoder_free(MRB, struct encoder *p)
{
    if (p->lz4f) {
        LZ4F_freeCompressionContext(p->lz4f);
    }

    mrb_free(mrb, p);
}

static const mrb_data_type encoder_type = {
    .struct_name = "mruby-lz4.encoder",
    .dfree = (void (*)(mrb_state *, void *))encoder_free,
};

static struct encoder *
getencoder(MRB, VALUE self)
{
    struct encoder *p;
    Data_Get_Struct(mrb, self, &encoder_type, p);
    return p;
}

static VALUE
encoder_set_outport(MRB, VALUE self, struct encoder *p, VALUE port)
{
    mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "mruby-lz4.outport"), port);
    p->io = port;
    return port;
}

static VALUE
encoder_set_outbuf(MRB, VALUE obj, struct encoder *p, VALUE buf)
{
    p->outbuf = buf;
    mrb_iv_set(mrb, obj, mrb_intern_lit(mrb, "mruby-lz4.outbuf"), buf);
    return buf;
}

/*
 * call-seq:
 *  new(outport, prefs = {})
 */
static VALUE
enc_s_new(MRB, VALUE self)
{
    struct RData *rd = mrb_data_object_alloc(mrb, mrb_class_ptr(self), NULL, &encoder_type);
    struct encoder *p = (struct encoder *)mrb_calloc(mrb, 1, sizeof(struct encoder));
    rd->data = p;
    LZ4F_errorCode_t err = LZ4F_createCompressionContext(&p->lz4f, LZ4F_getVersion());
    aux_lz4f_check_error(mrb, err, "LZ4F_createCompressionContext");
    p->io = Qnil;
    p->outbuf = Qnil;
    p->outbufsize = MIN(256 << 10, AUX_STR_MAX); /* AUX_STR_MAX or 256 KiB */

    VALUE obj = mrb_obj_value(rd);
    mrb_int argc;
    mrb_value *argv;
    mrb_get_args(mrb, "*", &argv, &argc);
    mrb_funcall_argv(mrb, obj, id_initialize, argc, argv);

    return obj;
}

static void
enc_initialize_args(MRB, VALUE *outport, LZ4F_preferences_t *prefs)
{
    mrb_int argc;
    VALUE *argv;
    mrb_get_args(mrb, "*", &argv, &argc);
    if (argc > 0 && mrb_hash_p(argv[argc - 1])) {
        *prefs = aux_lz4f_encode_args(mrb, argv[argc - 1]);
        argc --;
    } else {
        memset(prefs, 0, sizeof(*prefs));
    }

    if (argc == 1) {
        *outport = argv[0];
        return;
    } else {
        mrb_raisef(mrb,
                   E_ARGUMENT_ERROR,
                   "wrong number of arguments (given %S, expect 1 with keywords)",
                   mrb_fixnum_value(argc));
    }
}

static VALUE
enc_initialize(MRB, VALUE self)
{
    struct encoder *p = getencoder(mrb, self);
    enc_initialize_args(mrb, &p->io, &p->prefs);
    encoder_set_outport(mrb, self, p, p->io);

    encoder_set_outbuf(mrb, self, p, aux_str_buf_new(mrb, p->outbufsize));
    size_t s = LZ4F_compressBegin(p->lz4f, RSTRING_PTR(p->outbuf), RSTRING_CAPA(p->outbuf), &p->prefs);
    aux_lz4f_check_error(mrb, s, "LZ4F_compressBegin");
    mrbx_str_set_len(mrb, p->outbuf, s);
    FUNCALL(mrb, p->io, "<<", p->outbuf);

    return self;
}

/*
 * call-seq:
 *  write(src) -> self
 */
static VALUE
enc_write(MRB, VALUE self)
{
    struct encoder *p = getencoder(mrb, self);
    const char *src;
    mrb_int srclen;
    mrb_get_args(mrb, "s", &src, &srclen);

    const LZ4F_compressOptions_t opts = { .stableSrc = 0, };

    while (srclen > 0) {
        size_t insize = MIN(srclen, 4 * 1024 * 1024);
        size_t outsize = LZ4F_compressBound(insize, &p->prefs);
        encoder_set_outbuf(mrb, self, p, aux_str_alloc(mrb, p->outbuf, outsize));
        char *dest = RSTRING_PTR(p->outbuf);
        size_t s = LZ4F_compressUpdate(p->lz4f, dest, outsize, src, insize, &opts);
        aux_lz4f_check_error(mrb, s, "LZ4F_compressUpdate");
        mrbx_str_set_len(mrb, p->outbuf, s);
        FUNCALL(mrb, p->io, "<<", p->outbuf);
        src += insize;
        srclen -= insize;
    }

    return self;
}

/*
 * call-seq:
 *  flush -> self
 */
static VALUE
enc_flush(MRB, VALUE self)
{
    struct encoder *p = getencoder(mrb, self);
    const LZ4F_compressOptions_t opts = { .stableSrc = 0, };

    size_t insize = 4 * 1024 * 1024 + 16;
    size_t outsize = LZ4F_compressBound(insize, &p->prefs);
    encoder_set_outbuf(mrb, self, p, aux_str_alloc(mrb, p->outbuf, outsize));
    char *dest = RSTRING_PTR(p->outbuf);
    size_t s = LZ4F_flush(p->lz4f, dest, outsize, &opts);
    aux_lz4f_check_error(mrb, s, "LZ4F_flush");
    mrbx_str_set_len(mrb, p->outbuf, s);
    FUNCALL(mrb, p->io, "<<", p->outbuf);

    return self;
}

/*
 * call-seq:
 *  close -> self
 */
static VALUE
enc_close(MRB, VALUE self)
{
    struct encoder *p = getencoder(mrb, self);
    const LZ4F_compressOptions_t opts = { .stableSrc = 0, };

    size_t insize = 4 * 1024 * 1024 + 16;
    size_t outsize = LZ4F_compressBound(insize, &p->prefs);
    encoder_set_outbuf(mrb, self, p, aux_str_alloc(mrb, p->outbuf, outsize));
    char *dest = RSTRING_PTR(p->outbuf);
    size_t s = LZ4F_compressEnd(p->lz4f, dest, outsize, &opts);
    aux_lz4f_check_error(mrb, s, "LZ4F_compressEnd");
    mrbx_str_set_len(mrb, p->outbuf, s);
    FUNCALL(mrb, p->io, "<<", p->outbuf);

    return self;
}

/*
 * call-seq:
 *  port -> outport
 */
static VALUE
enc_get_port(MRB, VALUE self)
{
    return getencoder(mrb, self)->io;
}

static void
init_encoder(MRB, struct RClass *mLZ4)
{
    struct RClass *cEncoder = mrb_define_class_under(mrb, mLZ4, "Encoder", mrb_cObject);
    mrb_define_class_method(mrb, cEncoder, "encode", enc_s_encode, MRB_ARGS_ANY());
    mrb_define_class_method(mrb, cEncoder, "new", enc_s_new, MRB_ARGS_ANY());
    mrb_define_method(mrb, cEncoder, "initialize", enc_initialize, MRB_ARGS_ANY());
    mrb_define_method(mrb, cEncoder, "write", enc_write, MRB_ARGS_REQ(1));
    mrb_define_method(mrb, cEncoder, "flush", enc_flush, MRB_ARGS_NONE());
    mrb_define_method(mrb, cEncoder, "close", enc_close, MRB_ARGS_NONE());
    mrb_define_method(mrb, cEncoder, "port", enc_get_port, MRB_ARGS_NONE());

    mrb_define_alias(mrb, cEncoder, "<<", "write");
    mrb_define_alias(mrb, cEncoder, "finish", "close");
}

/*
 * class LZ4::Decoder
 */

static void
dec_s_decode_args(MRB, struct RString **src, struct RString **dest, ssize_t *maxdest)
{
    VALUE *argv;
    mrb_int argc;
    mrb_get_args(mrb, "*", &argv, &argc);

    switch (argc) {
    case 1:
        *maxdest = -1;
        *dest = NULL;
        break;
    case 2:
        if (mrb_string_p(argv[1])) {
            *dest = RString(argv[1]);
            *maxdest = -1;
        } else {
            *dest = NULL;
            *maxdest = aux_to_sizet(mrb, argv[1]);
        }
        break;
    case 3:
        *maxdest = aux_to_sizet(mrb, argv[1]);
        *dest = RString(argv[2]);
        break;
    default:
        mrb_raisef(mrb,
                   E_ARGUMENT_ERROR,
                   "wrong number of arguments (given %S, expect 1..3)",
                   mrb_fixnum_value(argc));
        break;
    }

    mrb_check_type(mrb, argv[0], MRB_TT_STRING);
    *src = RSTRING(argv[0]);

    size_t size = (*maxdest < 0 ? AUX_LZ4_DEFAULT_PARTIAL_SIZE : *maxdest);
    *dest = mrbx_str_force_recycle(mrb, *dest, size);
}

static void
dec_s_decode_all(MRB, VALUE self, struct RString *src, struct RString *dest, LZ4F_dctx *lz4f)
{
    LZ4F_decompressOptions_t opts = { .stableDst = 0, };
    size_t destoff = 0;
    size_t maxdest = 0;

    const char *srcp = RSTR_PTR(src);
    size_t srcsize = RSTR_LEN(src);

    for (;;) {
        if (!dest || destoff >= maxdest) {
            maxdest += AUX_LZ4_DEFAULT_PARTIAL_SIZE;
            dest = mrbx_str_reserve(mrb, dest, maxdest);
        }

        char *destp = RSTR_PTR(dest) + destoff;
        size_t destsize = maxdest - destoff;

        size_t s = LZ4F_decompress(lz4f, destp, &destsize, srcp, &srcsize, &opts);
        aux_lz4f_check_error(mrb, s, "LZ4F_decompress");
        destoff += destsize;
        srcp += srcsize;
        srcsize = RSTR_LEN(src) - (srcp - RSTR_PTR(src));

        if (s > srcsize) {
            mrb_raise(mrb, E_RUNTIME_ERROR, "``src'' is too small (unexpected termination)");
        }

        if (s == 0) { break; }
    }

    mrbx_str_set_len(mrb, dest, destoff);
}

static void
dec_s_decode_partial(MRB, VALUE self, struct RString *src, struct RString *dest, size_t maxdest, LZ4F_dctx *lz4f)
{
    LZ4F_decompressOptions_t opts = { .stableDst = 1, };

    const char *srcp = RSTR_PTR(src);
    size_t srcsize = RSTR_LEN(src);
    char *destp = RSTR_PTR(dest);
    size_t destsize = maxdest;

    size_t s = LZ4F_decompress(lz4f, destp, &destsize, srcp, &srcsize, &opts);
    aux_lz4f_check_error(mrb, s, "LZ4F_decompress");

    if (s > 0 && destsize < maxdest) {
        mrb_raise(mrb, E_RUNTIME_ERROR, "``src'' is too small (unexpected termination)");
    }

    mrbx_str_set_len(mrb, dest, destsize);
}

struct dec_s_decode
{
    VALUE self;
    struct RString *src, *dest;
    ssize_t maxdest;
    LZ4F_dctx *context;
};

static VALUE
dec_s_decode_try(MRB, VALUE argv)
{
    struct dec_s_decode *p = (struct dec_s_decode *)mrb_cptr(argv);

    if (p->maxdest < 0) {
        dec_s_decode_all(mrb, p->self, p->src, p->dest, p->context);
    } else {
        dec_s_decode_partial(mrb, p->self, p->src, p->dest, p->maxdest, p->context);
    }

    return VALUE(p->dest);
}

static VALUE
dec_s_decode_ensure(MRB, VALUE argv)
{
    struct dec_s_decode *p = (struct dec_s_decode *)mrb_cptr(argv);

    LZ4F_freeDecompressionContext(p->context);

    return Qnil;
}

/*
 * call-seq:
 *  decode(src, destsize = nil, dest = "") -> dest
 */
static VALUE
dec_s_decode(MRB, VALUE self)
{
    struct dec_s_decode args = { self, 0 };

    dec_s_decode_args(mrb, &args.src, &args.dest, &args.maxdest);

    size_t s = LZ4F_createDecompressionContext(&args.context, LZ4F_VERSION);
    aux_lz4f_check_error(mrb, s, "LZ4F_createDecompressionContext");

    return mrb_ensure(mrb,
            dec_s_decode_try, mrb_cptr_value(mrb, &args),
            dec_s_decode_ensure, mrb_cptr_value(mrb, &args));
}

struct decoder
{
    LZ4F_dctx *lz4f;
    VALUE predict;
    VALUE inport;
    VALUE inbuf;
    mrb_int inoff;
    mrb_int inbufsize;
};

static void
decoder_free(MRB, struct decoder *p)
{
    if (p->lz4f) {
        LZ4F_freeDecompressionContext(p->lz4f);
    }

    mrb_free(mrb, p);
}

static const mrb_data_type decoder_type = {
    .struct_name = "mruby-lz4.decoder",
    .dfree = (void (*)(mrb_state *, void *))decoder_free,
};

static struct decoder *
getdecoder(MRB, VALUE self)
{
    struct decoder *p;
    Data_Get_Struct(mrb, self, &decoder_type, p);
    return p;
}

static VALUE
decoder_set_predict(MRB, VALUE self, struct decoder *p, VALUE predict)
{
    mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "mruby-lz4.predict"), predict);
    p->predict = predict;
    return predict;
}

static VALUE
decoder_set_inport(MRB, VALUE self, struct decoder *p, VALUE port)
{
    mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "mruby-lz4.inport"), port);
    p->inport = port;
    return port;
}

static VALUE
decoder_set_inbuf(MRB, VALUE obj, struct decoder *p, VALUE buf)
{
    p->inbuf = buf;
    mrb_iv_set(mrb, obj, mrb_intern_lit(mrb, "mruby-lz4.inbuf"), buf);
    return buf;
}


/*
 * call-seq:
 *  new(inport)
 */
static VALUE
dec_s_new(MRB, VALUE self)
{
    struct RData *rd = mrb_data_object_alloc(mrb, mrb_class_ptr(self), NULL, &decoder_type);
    struct decoder *p = (struct decoder *)mrb_calloc(mrb, 1, sizeof(struct decoder));
    rd->data = p;

    LZ4F_errorCode_t err = LZ4F_createDecompressionContext(&p->lz4f, LZ4F_VERSION);
    aux_lz4f_check_error(mrb, err, "LZ4F_createDecompressionContext");
    p->inport = Qnil;
    p->inbuf = Qnil;
    p->inbufsize = MIN(1 << 20, AUX_STR_MAX); /* AUX_STR_MAX or 1 MiB */

    mrb_int argc;
    VALUE *argv;
    mrb_get_args(mrb, "*", &argv, &argc);
    mrb_funcall_argv(mrb, mrb_obj_value(rd), id_initialize, argc, argv);

    return mrb_obj_value(rd);
}

/*
 * call-seq:
 *  initialize(inport, opts = {})
 *
 * [opts (hash)]
 *
 *  predict (string OR nil)::
 *
 *      decompress with dictionary.
 */
static VALUE
dec_initialize(MRB, VALUE self)
{
    struct decoder *p = getdecoder(mrb, self);
    VALUE port, predict, opts;
    switch (mrb_get_args(mrb, "o|H", &port, &opts)) {
    case 1:
        predict = Qnil;
        break;
    case 2:
        MRBX_SCANHASH(mrb, opts, Qnil,
                MRBX_SCANHASH_ARGS("predict", &predict, Qnil));
        if (!NIL_P(predict)) { mrb_check_type(mrb, predict, MRB_TT_STRING); }
        break;
    default:
        AUX_NOT_REACHED_HERE;
    }

    decoder_set_inport(mrb, self, p, port);
    decoder_set_predict(mrb, self, p, predict);

    if (mrb_string_p(p->inport)) {
        decoder_set_inbuf(mrb, self, p, port);
        p->inbufsize = -1;
    } else {
        p->inbufsize = AUX_LZ4_DEFAULT_PARTIAL_SIZE;
    }

    return self;
}

static int
dec_read_fetch(MRB, VALUE self, struct decoder *p)
{
    if (p->inoff >= RSTRING_LEN(p->inbuf)) {
        if (p->inbufsize < 1) { p->inbufsize = 0; return -1; }

        VALUE v = FUNCALL(mrb, p->inport, "read", mrb_fixnum_value(p->inbufsize), p->inbuf);
        if (NIL_P(v)) { p->inbufsize = 0; return -1; }
        mrb_check_type(mrb, v, MRB_TT_STRING);
        if (RSTRING_LEN(v) < 1) { p->inbufsize = 0; return -1; }
        if (mrb_obj_eq(mrb, v, p->inbuf)) {
            decoder_set_inbuf(mrb, self, p, v);
        }

        p->inoff = 0;
    }

    return 0;
}

/*
 * call-seq:
 *  read(size = nil, dest = "") -> dest
 */
static VALUE
dec_read(MRB, VALUE self)
{
    struct decoder *p = getdecoder(mrb, self);
    intptr_t size;
    struct RString *dest;
    common_read_args(mrb, &size, &dest);
    if (size == 0) { return VALUE(dest); }

    int arena = mrb_gc_arena_save(mrb);

    while (size < 0 || RSTR_LEN(dest) < size) {
        mrb_gc_arena_restore(mrb, arena);

        if (dec_read_fetch(mrb, self, p) < 0) {
            break;
        }

        const char *srcp = RSTRING_PTR(p->inbuf) + p->inoff;
        size_t srcsize = RSTRING_LEN(p->inbuf) - p->inoff;
        char *destp = RSTR_PTR(dest) + RSTR_LEN(dest);
        size_t destsize = (size < 0 ? RSTR_CAPA(dest) : size) - RSTR_LEN(dest);
        size_t s = LZ4F_decompress(p->lz4f, destp, &destsize, srcp, &srcsize, NULL);
        p->inoff += srcsize;
        RSTR_SET_LEN(dest, RSTR_LEN(dest) + destsize);
        aux_lz4f_check_error(mrb, s, "LZ4F_decompress");
        if (s == 0 || RSTR_LEN(dest) >= AUX_STR_MAX) {
            break;
        }

        if (size < 0 && RSTR_LEN(dest) >= RSTR_CAPA(dest)) {
            size_t capa = RSTR_CAPA(dest) + AUX_LZ4_DEFAULT_PARTIAL_SIZE;
            capa = MIN(capa, AUX_STR_MAX);
            mrbx_str_reserve(mrb, dest, capa);
        }
    }

    if (RSTR_LEN(dest) > 0) {
        return VALUE(dest);
    } else {
        return Qnil;
    }
}

/*
 * call-seq:
 *  close -> nil
 */
static VALUE
dec_close(MRB, VALUE self)
{
    getdecoder(mrb, self)->inbufsize = 0;

    return Qnil;
}

/*
 * call-seq:
 *  eof -> true OR false
 */
static VALUE
dec_eof(MRB, VALUE self)
{
    if (getdecoder(mrb, self)->inbufsize > 0) {
        return Qfalse;
    } else {
        return Qtrue;
    }
}

/*
 * call-seq:
 *  port -> inport
 */
static VALUE
dec_get_port(MRB, VALUE self)
{
    return getdecoder(mrb, self)->inport;
}

static void
init_decoder(MRB, struct RClass *mLZ4)
{
    struct RClass *cDecoder = mrb_define_class_under(mrb, mLZ4, "Decoder", mrb_cObject);
    MRB_SET_INSTANCE_TT(cDecoder, MRB_TT_DATA);

    mrb_define_class_method(mrb, cDecoder, "decode", dec_s_decode, MRB_ARGS_ANY());
    mrb_define_class_method(mrb, cDecoder, "new", dec_s_new, MRB_ARGS_ANY());
    mrb_define_method(mrb, cDecoder, "initialize", dec_initialize, MRB_ARGS_ANY());
    mrb_define_method(mrb, cDecoder, "read", dec_read, MRB_ARGS_ANY());
    mrb_define_method(mrb, cDecoder, "close", dec_close, MRB_ARGS_NONE());
    mrb_define_method(mrb, cDecoder, "eof", dec_eof, MRB_ARGS_NONE());
    mrb_define_method(mrb, cDecoder, "port", dec_get_port, MRB_ARGS_NONE());

    mrb_define_alias(mrb, cDecoder, "finish", "close");
    mrb_define_alias(mrb, cDecoder, "eof?", "eof");
}

#endif /* MRB_INT16 */

/*
 * class LZ4::BlockEncoder
 */

static void
aux_LZ4_resetStream(void *cx, int level)
{
    LZ4_resetStream((LZ4_stream_t *)cx);
}

static int
aux_LZ4_loadDict(void *cx, const char *predict, size_t dictsize)
{
    return LZ4_loadDict((LZ4_stream_t *)cx, predict, dictsize);
}

static int
aux_LZ4_saveDict(void *cx, char *predict, size_t dictsize)
{
    return LZ4_saveDict((LZ4_stream_t *)cx, predict, dictsize);
}

static int
aux_LZ4_compress_fast_continue(void *cx, const char *src, char *dest, size_t srcsize, size_t destsize, int level)
{
    return LZ4_compress_fast_continue((LZ4_stream_t *)cx, src, dest, srcsize, destsize, -level);
}

static void
aux_LZ4_resetStreamHC(void *cx, int level)
{
    LZ4_resetStreamHC((LZ4_streamHC_t *)cx, level);
}

static int
aux_LZ4_loadDictHC(void *cx, const char *predict, size_t dictsize)
{
    return LZ4_loadDictHC((LZ4_streamHC_t *)cx, predict, dictsize);
}

static int
aux_LZ4_saveDictHC(void *cx, char *predict, size_t dictsize)
{
    return LZ4_saveDictHC((LZ4_streamHC_t *)cx, predict, dictsize);
}

static int
aux_LZ4_compress_HC_continue(void *cx, const char *src, char *dest, size_t srcsize, size_t destsize, int level)
{
    return LZ4_compress_HC_continue((LZ4_streamHC_t *)cx, src, dest, srcsize, destsize);
}

struct block_encoder_traits
{
    const char *compress_continue_name;
    size_t context_size;
    void (*reset_stream)(void *, int);
    int (*load_dict)(void *, const char *, size_t);
    int (*save_dict)(void *, char *, size_t);
    int (*compress_continue)(void *, const char *, char *, size_t, size_t, int);
};

static const struct
{
    struct block_encoder_traits fast;
    struct block_encoder_traits hc;
} block_encoder_traits = {
    { "LZ4_compress_fast_continue", sizeof(LZ4_stream_t), aux_LZ4_resetStream, aux_LZ4_loadDict, aux_LZ4_saveDict, aux_LZ4_compress_fast_continue },
    { "LZ4_compress_HC_continue", sizeof(LZ4_streamHC_t), aux_LZ4_resetStreamHC, aux_LZ4_loadDictHC, aux_LZ4_saveDictHC, aux_LZ4_compress_HC_continue },
};

struct block_encoder
{
    const struct block_encoder_traits *traits;

    int level;

    char *prefix;
    size_t prefix_length;
    size_t prefix_capacity;

    void *lz4;

    /* 直後の連続した領域に prefix と lz4 が確保される */
};

static const mrb_data_type block_encoder_type = {
    "LZ4::BlockEncoder@mruby-lz4",
    mrb_free,
};

static struct block_encoder *
get_block_encoder(MRB, VALUE self)
{
    struct block_encoder *p = (struct block_encoder *)mrb_data_get_ptr(mrb, self, &block_encoder_type);

    if (!p) {
        mrb_raisef(mrb, E_TYPE_ERROR,
                   "wrong initialized - %S", self);
    }

    return p;
}

static void
blkenc_initialize_args(MRB, const struct block_encoder_traits **traits, mrb_int *level, struct RString **predict, mrb_int *precapa)
{
    mrb_int argc;
    VALUE *argv;

    switch (mrb_get_args(mrb, "*", &argv, &argc)) {
    case 0:
        *level = convert_to_lz4_level(mrb, Qnil);
        *predict = NULL;
        *precapa = convert_to_prefix_capacity(mrb, Qnil);
        break;
    case 1:
        *level = convert_to_lz4_level(mrb, argv[0]);
        *predict = NULL;
        *precapa = convert_to_prefix_capacity(mrb, Qnil);
        break;
    case 2:
        *level = convert_to_lz4_level(mrb, argv[0]);
        *predict = RString(argv[1]);
        *precapa = convert_to_prefix_capacity(mrb, Qnil);
        break;
    case 3:
        *level = convert_to_lz4_level(mrb, argv[0]);
        *predict = RString(argv[1]);
        *precapa = convert_to_prefix_capacity(mrb, argv[2]);
        break;
    default:
        mrbx_error_arity(mrb, argc, 0, 3);
    }

    if (*level < 0) {
        *traits = &block_encoder_traits.fast;
    } else {
        *traits = &block_encoder_traits.hc;
    }

    if (*predict && RSTR_LEN(*predict) > *precapa) {
        mrb_raise(mrb, E_RUNTIME_ERROR,
                  "predict を指定されたが、prefix_capacity が小さすぎる");
    }
}

static VALUE
blkenc_initialize(MRB, VALUE self)
{
    mrb_int level, precapa;
    struct RString *predict;
    const struct block_encoder_traits *traits;
    blkenc_initialize_args(mrb, &traits, &level, &predict, &precapa);

    if (DATA_PTR(self) || DATA_TYPE(self)) {
        mrb_raisef(mrb, E_TYPE_ERROR,
                   "already initialized - %S",
                   self);
    }

    /* XXX: アライメントの考慮が必要なのかはわからない */
    size_t size = sizeof(struct block_encoder) + traits->context_size + precapa;

    struct block_encoder *p = (struct block_encoder *)mrb_malloc(mrb, size);

    memset(p, 0, size);
    p->traits = traits;
    p->level = level;
    p->lz4 = (void *)((char *)p + sizeof(*p));
    p->prefix = (char *)p + traits->context_size;
    p->prefix_capacity = precapa;

    traits->reset_stream(p->lz4, level);

    if (predict) {
        traits->load_dict(p->lz4, RSTR_PTR(predict), RSTR_LEN(predict));
        traits->save_dict(p->lz4, p->prefix, p->prefix_capacity);
    }

    mrb_data_init(self, p, &block_encoder_type);

    return self;
}

static void
blkenc_reset_args(MRB, VALUE self, struct block_encoder **p, mrb_int *level, struct RString **dict)
{
    VALUE *argv;
    mrb_int argc;
    mrb_get_args(mrb, "*", &argv, &argc);

    *p = get_block_encoder(mrb, self);

    switch (argc) {
    case 0:
        *level = (*p)->level;
        *dict = NULL;
        break;
    case 1:
        *level = (NIL_P(argv[0]) ? (*p)->level : mrb_int(mrb, argv[0]));
        *dict = NULL;
        break;
    case 2:
        *level = (NIL_P(argv[0]) ? (*p)->level : mrb_int(mrb, argv[0]));
        *dict = RString(argv[1]);
        break;
    default:
        mrbx_error_arity(mrb, argc, 0, 2);
    }

    if ((*level < 0 && (*p)->level >= 0) || (*level >= 0 && (*p)->level < 0)) {
        mrb_raise(mrb, E_ARGUMENT_ERROR,
                  "wrong level (both encoder level and argument level are make up the number sign)");
    }
}

static VALUE
blkenc_reset(MRB, VALUE self)
{
    mrb_int level;
    struct RString *dict;
    struct block_encoder *p;
    blkenc_reset_args(mrb, self, &p, &level, &dict);

    p->traits->reset_stream(p->lz4, level);
    p->level = level;

    if (dict) {
        p->traits->load_dict(p->lz4, RSTR_PTR(dict), RSTR_LEN(dict));
        p->traits->save_dict(p->lz4, p->prefix, p->prefix_capacity);
    }

    return self;
}

static void
blkenc_encode_args(MRB, VALUE self, struct block_encoder **p, char **srcp, mrb_int *srclen, mrb_int *maxdest, struct RString **dest)
{
    mrb_int argc;
    VALUE *argv;
    mrb_get_args(mrb, "s*", srcp, srclen, &argv, &argc);

    switch (argc) {
    case 0:
        *maxdest = -1;
        *dest = NULL;
        break;
    case 1:
        *maxdest = (NIL_P(argv[0]) ? -1 : mrb_int(mrb, argv[0]));
        *dest = NULL;
        break;
    case 2:
        *maxdest = (NIL_P(argv[0]) ? -1 : mrb_int(mrb, argv[0]));
        *dest = RString(argv[1]);
        break;
    default:
        mrbx_error_arity(mrb, argc + 1, 1, 3);
    }

    if (*maxdest < 0) {
        *maxdest = LZ4_compressBound(*srclen);
    }

    *dest = mrbx_str_force_recycle(mrb, *dest, *maxdest);

    *p = get_block_encoder(mrb, self);
}

static VALUE
blkenc_encode(MRB, VALUE self)
{
    char *srcp;
    mrb_int srclen;
    struct RString *dest;
    mrb_int maxdest;
    struct block_encoder *p;
    blkenc_encode_args(mrb, self, &p, &srcp, &srclen, &maxdest, &dest);

    int s = p->traits->compress_continue(p->lz4, srcp, RSTR_PTR(dest), srclen, maxdest, p->level);

    if (s <= 0) {
        mrb_raisef(mrb, E_RUNTIME_ERROR,
                   "%S failed (code:%S)",
                   VALUE(p->traits->compress_continue_name),
                   VALUE((mrb_int)s));
    }
    mrbx_str_set_len(mrb, dest, s);

    if ((p->prefix_length = p->traits->save_dict(p->lz4, p->prefix, p->prefix_capacity)) == 0) {
        /* NOTE: 保存に失敗したため、リンクを切る */
        p->traits->load_dict(p->lz4, NULL, 0);
    } else {
        p->traits->load_dict(p->lz4, p->prefix, p->prefix_length);
    }

    return VALUE(dest);
}

/*
 * call-seq:
 *  encode_size(src) -> unsigned integer (OR float)
 */
static VALUE
blkenc_s_encode_size(MRB, VALUE self)
{
    VALUE src;
    mrb_get_args(mrb, "o", &src);
    size_t size;
    if (mrb_string_p(src)) {
        size = RSTRING_LEN(src);
    } else if (mrb_float_p(src)) {
        size = CLAMP(mrb_float(src), 0, UINT32_MAX);
    } else {
        size = CLAMP(mrb_int(mrb, src), 0, UINT32_MAX);
    }

    size = LZ4_compressBound(size);
    if (size > MRB_INT_MAX) {
        return mrb_float_value(mrb, size);
    } else {
        return mrb_fixnum_value(size);
    }
}

static void
blkenc_s_encode_args(MRB, struct RString **src, struct RString **dest, size_t *maxdest, int *level, struct RString **predict)
{
    mrb_int argc;
    VALUE *argv;
    mrb_get_args(mrb, "*", &argv, &argc);
    if (argc > 0 && mrb_hash_p(argv[argc - 1])) {
        VALUE alevel, apredict;
        MRBX_SCANHASH(mrb, argv[argc - 1], Qnil,
                MRBX_SCANHASH_ARGS("level", &alevel, Qnil),
                MRBX_SCANHASH_ARGS("predict", &apredict, Qnil));

        *level = (NIL_P(alevel) ? -1 : mrb_int(mrb, alevel));
        *predict = RString(apredict);

        argc --;
    } else {
        *level = -1;
        *predict = NULL;
    }

    switch (argc) {
    case 1:
        *maxdest = -1;
        *dest = NULL;
        break;
    case 2:
        if (NIL_P(argv[1])) {
            *maxdest = -1;
            *dest = NULL;
        } else if (mrb_string_p(argv[1])) {
            *maxdest = -1;
            *dest = RSTRING(argv[1]);
        } else {
            *maxdest = aux_to_u32(mrb, argv[1]);
            *dest = NULL;
        }
        break;
    case 3:
        *maxdest = aux_to_u32(mrb, argv[1]);
        *dest = RString(argv[2]);
        break;
    default:
        mrb_raisef(mrb,
                   E_ARGUMENT_ERROR,
                   "wrong number of arguments (given %S, expect 1..3 + keywords)",
                   mrb_fixnum_value(argc));
    }

    mrb_check_type(mrb, argv[0], MRB_TT_STRING);
    *src = RSTRING(argv[0]);

    if (*maxdest == -1) {
        *maxdest = LZ4_compressBound(RSTR_LEN(*src));
    }

    if (*maxdest > AUX_STR_MAX) {
        mrb_raise(mrb, E_RUNTIME_ERROR,
                  "maxdest (or compress bound) is too large");
    }

    *dest = mrbx_str_force_recycle(mrb, *dest, *maxdest);
    mrbx_str_set_len(mrb, *dest, 0);
}

/*
 * call-seq:
 *  encode(src, maxsize = nil, dest = "", opts = {}) -> dest
 *  encode(src, dest, opts = {}) -> dest
 *
 * [opts (hash)]
 *
 *  level (nil OR signed integer)::
 *
 *      Choose compression level.
 *
 *      * ``nil`` is fast compression (default, means -1)
 *      * negative is faster compression
 *      * ``0`` is high compression
 *      * positive is higher compression
 *
 *  predict (string OR nil)::
 *
 *      compression with dictionary
 */
static VALUE
blkenc_s_encode(MRB, VALUE self)
{
    struct RString *src, *dest, *predict;
    size_t maxdest;
    int level;
    blkenc_s_encode_args(mrb, &src, &dest, &maxdest, &level, &predict);

    const struct block_encoder_traits *traits;

    if (level < 0) {
        traits = &block_encoder_traits.fast;
    } else {
        traits = &block_encoder_traits.hc;
    }

    void *lz4 = mrb_malloc(mrb, traits->context_size);
    traits->reset_stream(lz4, level);
    if (predict) {
        traits->load_dict(lz4, RSTR_PTR(predict), RSTR_LEN(predict));
    }

    int s = traits->compress_continue(lz4, RSTR_PTR(src), RSTR_PTR(dest), RSTR_LEN(src), maxdest, level);
    mrb_free(mrb, lz4);
    if (s <= 0) {
        mrb_raisef(mrb, E_RUNTIME_ERROR,
                   "%S failed (code:%S)",
                   VALUE(traits->compress_continue_name),
                   VALUE((mrb_int)s));
    }
    mrbx_str_set_len(mrb, dest, s);

    return VALUE(dest);
}

static void
init_block_encoder(MRB, struct RClass *mLZ4)
{
    struct RClass *cBlockEncoder = mrb_define_class_under(mrb, mLZ4, "BlockEncoder", mrb_cObject);
    mrb_define_class_method(mrb, cBlockEncoder, "encode_size", blkenc_s_encode_size, MRB_ARGS_REQ(1));
    mrb_define_class_method(mrb, cBlockEncoder, "encode", blkenc_s_encode, MRB_ARGS_ANY());

    MRB_SET_INSTANCE_TT(cBlockEncoder, MRB_TT_DATA);
    mrb_define_method(mrb, cBlockEncoder, "initialize", blkenc_initialize, MRB_ARGS_ANY());
    mrb_define_method(mrb, cBlockEncoder, "encode", blkenc_encode, MRB_ARGS_ANY());
    mrb_define_method(mrb, cBlockEncoder, "reset", blkenc_reset, MRB_ARGS_ANY());

    mrb_define_const(mrb, cBlockEncoder, "LZ4HC_CLEVEL_MIN", mrb_fixnum_value(LZ4HC_CLEVEL_MIN));
    mrb_define_const(mrb, cBlockEncoder, "LZ4HC_CLEVEL_DEFAULT", mrb_fixnum_value(LZ4HC_CLEVEL_DEFAULT));
    mrb_define_const(mrb, cBlockEncoder, "LZ4HC_CLEVEL_OPT_MIN", mrb_fixnum_value(LZ4HC_CLEVEL_OPT_MIN));
    mrb_define_const(mrb, cBlockEncoder, "LZ4HC_CLEVEL_MAX", mrb_fixnum_value(LZ4HC_CLEVEL_MAX));
    mrb_define_const(mrb, cBlockEncoder, "LZ4_MAX_INPUT_SIZE", mrb_fixnum_value(LZ4_MAX_INPUT_SIZE));
    mrb_define_const(mrb, cBlockEncoder, "HCLEVEL_MIN", mrb_fixnum_value(LZ4HC_CLEVEL_MIN));
    mrb_define_const(mrb, cBlockEncoder, "HCLEVEL_DEFAULT", mrb_fixnum_value(LZ4HC_CLEVEL_DEFAULT));
    mrb_define_const(mrb, cBlockEncoder, "HCLEVEL_OPT_MIN", mrb_fixnum_value(LZ4HC_CLEVEL_OPT_MIN));
    mrb_define_const(mrb, cBlockEncoder, "HCLEVEL_MAX", mrb_fixnum_value(LZ4HC_CLEVEL_MAX));
    mrb_define_const(mrb, cBlockEncoder, "MAX_INPUT_SIZE", mrb_fixnum_value(LZ4_MAX_INPUT_SIZE));
    mrb_define_const(mrb, cBlockEncoder, "ACCELERATION_DEFAULT", mrb_fixnum_value(-1)); /* see lib/lz4.c in liblz4 */
}

/*
 * class LZ4::BlockDecoder
 */

/*
 * call-seq:
 *  initialize(predict = nil, prefix_capacity = <AUX_LZ4_PREFIX_MAX_CAPACITY>)
 */
static VALUE
blkdec_initialize(MRB, VALUE self)
{
    char *predictp = NULL;
    mrb_int predictlen = 0;
    mrb_int prefixcapa = AUX_LZ4_PREFIX_MAX_CAPACITY;
    mrb_get_args(mrb, "|s!i", &predictp, &predictlen, &prefixcapa);

    /* TODO: predictlen を AUX_LZ4_PREFIX_MAX_CAPACITY に切り詰める */

    if (prefixcapa < 0) {
        prefixcapa = AUX_LZ4_PREFIX_MAX_CAPACITY;
    }

    if (predictlen > prefixcapa) {
        mrb_raisef(mrb, E_ARGUMENT_ERROR,
                   "wrong predict length - prefix buffer overflow (predict length=%S, prefix capacity=%S)",
                   VALUE(predictlen), VALUE(prefixcapa));
    }

    mrb_str_modify(mrb, RSTRING(self));
    mrb_str_resize(mrb, self, prefixcapa);
    RSTR_SET_LEN(RSTRING(self), 0);
    aux_str_cat(mrb, RSTRING(self), predictp, predictlen);

    return self;
}

/*
 * call-seq:
 *  decode(src, destmax = nil, dest = nil) -> dest or string
 */
static VALUE
blkdec_decode(MRB, VALUE self)
{
    char *srcp;
    mrb_int srclen;
    mrb_int destmax = -1;
    VALUE destv = Qnil;
    mrb_get_args(mrb, "s!|iS!", &srcp, &srclen, &destmax, &destv);

    if (destmax < 0) {
        size_t size = aux_lz4_scan_size(mrb, srcp, srclen);
        if (size > MRBX_STR_MAX) {
            mrb_raise(mrb, E_RUNTIME_ERROR,
                      "maybe out of memory for decompression data");
        }

        destmax = size;
    }

    struct RString *dest = mrbx_str_force_recycle(mrb, destv, destmax);
    struct RString *selfp = RSTRING(self);

    int destlen = LZ4_decompress_safe_usingDict(srcp, RSTR_PTR(dest), srclen, destmax, RSTR_PTR(selfp), RSTR_LEN(selfp));
    if (destlen < 0) {
        mrb_raisef(mrb, E_RUNTIME_ERROR, "LZ4_decompress_safe_usingDict failed (%S)", mrb_fixnum_value(destlen));
    }
    RSTR_SET_LEN(dest, destlen);

    mrb_int selfcapa = RSTR_CAPA(selfp);
    if (destlen >= selfcapa) {
        RSTR_SET_LEN(selfp, 0);
        aux_str_cat(mrb, selfp, RSTR_PTR(dest) + destlen - selfcapa, selfcapa);
    } else {
        mrb_int cutlen = RSTR_LEN(selfp) - (selfcapa - destlen);

        if (cutlen > 0) {
            mrbx_str_drop(mrb, selfp, 0, cutlen);
        }

        aux_str_cat(mrb, selfp, RSTR_PTR(dest), destlen);
    }

    return VALUE(dest);
}

static VALUE
blkdec_reset(MRB, VALUE self)
{
    mrb_get_args(mrb, "");

    RSTR_SET_LEN(RSTRING(self), 0);

    return self;
}

/*
 * call-seq:
 *  decode_size(src) -> unsigned integer (OR float)
 */
static VALUE
blkdec_s_decode_size(MRB, VALUE self)
{
    VALUE src;
    mrb_get_args(mrb, "S", &src);

    uint32_t size = aux_lz4_scan_size(mrb, RSTRING_PTR(src), RSTRING_LEN(src));
    if ((uint64_t)size > (uint64_t)MRB_INT_MAX) {
        return mrb_float_value(mrb, size);
    } else {
        return mrb_fixnum_value(size);
    }
}

static void
blkdec_s_decode_args(MRB, VALUE *src, VALUE *dest, VALUE *predict)
{
    mrb_int argc;
    VALUE *argv;
    mrb_get_args(mrb, "*", &argv, &argc);
    if (argc > 0 && mrb_hash_p(argv[argc - 1])) {
        MRBX_SCANHASH(mrb, argv[argc - 1], Qnil,
                MRBX_SCANHASH_ARGS("predict", predict, Qnil));
        argc --;
        if (!NIL_P(*predict)) {
            mrb_check_type(mrb, *predict, MRB_TT_STRING);
        }
    } else {
        *predict = Qnil;
    }

    int32_t maxdest;
    switch (argc) {
    case 1:
        maxdest = -1;
        *dest = Qnil;
        break;
    case 2:
        if (mrb_string_p(argv[1])) {
            maxdest = -1;
            *dest = argv[1];
        } else {
            maxdest = aux_to_u32(mrb, argv[1]);
            *dest = Qnil;
        }
        break;
    case 3:
        maxdest = aux_to_u32(mrb, argv[1]);
        *dest = argv[2];
        break;
    default:
        mrb_raisef(mrb,
                   E_ARGUMENT_ERROR,
                   "wrong number of arguments (given %S, expect 1..3 + keywords)",
                   mrb_fixnum_value(argc));
    }

    *src = argv[0];
    mrb_check_type(mrb, *src, MRB_TT_STRING);

    if (maxdest == -1) {
        maxdest = aux_lz4_scan_size(mrb, RSTRING_PTR(*src), RSTRING_LEN(*src));
    }
    maxdest = (int32_t)MIN((int64_t)maxdest, (int64_t)AUX_STR_MAX);

    if (NIL_P(*dest)) {
        *dest = aux_str_buf_new(mrb, maxdest);
    } else {
        mrb_check_type(mrb, *dest, MRB_TT_STRING);
    }
    mrbx_str_reserve(mrb, *dest, maxdest);
}

/*
 * call-seq:
 *  decode(src, maxsize = nil, dest = "", opts = {}) -> dest
 *  decode(src, dest, opts = {}) -> dest
 *
 * [opts (hash)]
 *  predict (string OR nil):: decompression with dictionary
 */
static VALUE
blkdec_s_decode(MRB, VALUE self)
{
    VALUE src, dest, predict;
    blkdec_s_decode_args(mrb, &src, &dest, &predict);

    LZ4_streamDecode_t *lz4 = (LZ4_streamDecode_t *)mrb_calloc(mrb, 1, sizeof(LZ4_streamDecode_t));
    if (NIL_P(predict)) {
        LZ4_setStreamDecode(lz4, NULL, 0);
    } else {
        LZ4_setStreamDecode(lz4, RSTRING_PTR(predict), RSTRING_LEN(predict));
    }
    int s = LZ4_decompress_safe_continue(lz4, RSTRING_PTR(src), RSTRING_PTR(dest), RSTRING_LEN(src), RSTRING_CAPA(dest));
    mrb_free(mrb, lz4);
    if (s < 0) {
        mrb_raise(mrb, E_RUNTIME_ERROR, "LZ4_decompress_safe_continue failed");
    }
    mrbx_str_set_len(mrb, dest, s);

    return dest;
}

#ifndef WITHOUT_UNLZ4_GRADUAL
#include "unlz4-gradual.h"

#ifdef MRB_INT16
# define AUX_PARTIAL_READ_SIZE (4 << 10) /* 4 KiB */
#else
# define AUX_PARTIAL_READ_SIZE (16 << 10) /* 16 KiB */
#endif

static void
aux_unlz4_gradual_check_error(MRB, enum unlz4_gradual_status status, const char mesg[])
{
    if (status > UNLZ4_GRADUAL_OK) {
        if (mesg) {
            mrb_raisef(mrb, E_RUNTIME_ERROR,
                       "failed %S - %S (%S)",
                       VALUE(mesg),
                       VALUE(unlz4_gradual_str_status(status)),
                       VALUE((mrb_int)status));
        } else {
            mrb_raisef(mrb, E_RUNTIME_ERROR,
                       "unlz4-gradual error - %S (%S)",
                       VALUE(unlz4_gradual_str_status(status)),
                       VALUE((mrb_int)status));
        }
    }
}

struct unlz4g
{
    struct unlz4_gradual *unlz4;
    enum unlz4_gradual_status status;
};

static void
unlz4g_free(MRB, struct unlz4g *g)
{
    if (g) {
        mrb_free(mrb, g->unlz4);
        mrb_free(mrb, g);
    }
}

static const mrb_data_type unlz4g_type = {
    .struct_name = "unlz4-gradual@mruby-lz4",
    .dfree = (void (*)(mrb_state *, void *))unlz4g_free,
};

static void
unlz4g_initialize_args(MRB, VALUE self, VALUE *inport, uint32_t *prefix_capacity, struct RString **predict)
{
    mrb_int argc;
    VALUE *argv;
    mrb_get_args(mrb, "*", &argv, &argc);

    VALUE opts;
    if (argc > 0 && mrb_obj_is_kind_of(mrb, argv[argc - 1], mrb->hash_class)) {
        opts = argv[-- argc];
    } else {
        opts = Qnil;
    }

    switch (argc) {
    case 1:
        *inport = argv[0];
        *prefix_capacity = 65536;
        break;
    case 2:
        *inport = argv[0];
        *prefix_capacity = mrb_int(mrb, argv[1]);
        break;
    default:
        mrbx_error_arity(mrb, argc, 1, 2);
    }

    VALUE predictv;
    MRBX_SCANHASH(mrb, opts, Qnil,
            MRBX_SCANHASH_ARGS("predict", &predictv, Qnil));

    *predict = mrbx_str_ptr(mrb, predictv);
}

/*
 * call-seq:
 *  initialize(inport, prefix_capacity = 65536, predict: nil)
 */
static VALUE
unlz4g_initialize(MRB, VALUE self)
{
    VALUE inport;
    uint32_t prefix_capacity;
    struct RString *predict;
    unlz4g_initialize_args(mrb, self, &inport, &prefix_capacity, &predict);

    if (mrb_data_check_get_ptr(mrb, self, &unlz4g_type)) {
        mrb_raisef(mrb, E_RUNTIME_ERROR,
                   "wrong initialized again - %S",
                   mrb_any_to_s(mrb, self));
    }

    struct RData *da = RDATA(self);
    da->type = &unlz4g_type;
    struct unlz4g *g = (struct unlz4g *)(da->data = mrb_malloc(mrb, sizeof(struct unlz4g)));
    memset(g, 0, sizeof(*g));
    enum unlz4_gradual_status s = unlz4_gradual_alloc(&g->unlz4, prefix_capacity, (void *(*)(void *, size_t))mrb_malloc, mrb);
    aux_unlz4_gradual_check_error(mrb, s, "unlz4_gradual_alloc");
    if (predict) {
        s = unlz4_gradual_reset(g->unlz4, RSTR_PTR(predict), RSTR_LEN(predict));
        aux_unlz4_gradual_check_error(mrb, s, "unlz4_gradual_reset");
    }
    mrb_iv_set(mrb, self, id_ivar_inport, mrbx_fakedin_new(mrb, inport));

    return self;
}

static VALUE
unlz4g_read(MRB, VALUE self)
{
    struct RString *dest;
    ssize_t maxdest;
    common_read_args(mrb, &maxdest, &dest);
    struct unlz4g *g = (struct unlz4g *)mrbx_getref(mrb, self, &unlz4g_type);
    VALUE inport = mrb_iv_get(mrb, self, id_ivar_inport);
    if (maxdest < 0) { mrb_raise(mrb, E_NOTIMP_ERROR, "!"); }

    g->unlz4->next_out = RSTR_PTR(dest);
    g->unlz4->avail_out = maxdest;

    while (g->unlz4->avail_out > 0) {
        if (g->status == UNLZ4_GRADUAL_NEED_INPUT ||
                g->status == UNLZ4_GRADUAL_MAYBE_FINISHED) {
            g->unlz4->avail_in = mrbx_fakedin_read(mrb, inport, &g->unlz4->next_in, AUX_PARTIAL_READ_SIZE);

            if (g->unlz4->avail_in < 0) {
                if (g->status == UNLZ4_GRADUAL_MAYBE_FINISHED) {
                    break;
                } else {
                    mrb_raise(mrb, E_RUNTIME_ERROR, "unexpected end of stream");
                }
            }
        }

        g->status = unlz4_gradual(g->unlz4);
        if (g->status > UNLZ4_GRADUAL_OK) {
            aux_unlz4_gradual_check_error(mrb, g->status, "unlz4_gradual");
        }
    }

    mrbx_str_set_len(mrb, dest, maxdest - g->unlz4->avail_out);

    return (RSTR_LEN(dest) > 0 ? VALUE(dest) : Qnil);
}

//unlz4g_close

static VALUE
unlz4g_eof(MRB, VALUE self)
{
    return mrb_bool_value(((struct unlz4g *)mrbx_getref(mrb, self, &unlz4g_type))->status == UNLZ4_GRADUAL_MAYBE_FINISHED);
}
#endif /* WITHOUT_UNLZ4_GRADUAL */

static void
init_block_decoder(MRB, struct RClass *mLZ4)
{
    struct RClass *cBlockDecoder = mrb_define_class_under(mrb, mLZ4, "BlockDecoder", mrb_cObject);
    mrb_define_class_method(mrb, cBlockDecoder, "decode_size", blkdec_s_decode_size, MRB_ARGS_REQ(1));
    mrb_define_class_method(mrb, cBlockDecoder, "decode", blkdec_s_decode, MRB_ARGS_ANY());

    /*
     * どうせ prefix (dictionary) バッファのみしか保持しないため、string で事足りる。
     */
    MRB_SET_INSTANCE_TT(cBlockDecoder, MRB_TT_STRING);
    mrb_define_method(mrb, cBlockDecoder, "initialize", blkdec_initialize, MRB_ARGS_ANY());
    mrb_define_method(mrb, cBlockDecoder, "decode", blkdec_decode, MRB_ARGS_ANY());
    mrb_define_method(mrb, cBlockDecoder, "reset", blkdec_reset, MRB_ARGS_ANY());

#ifndef WITHOUT_UNLZ4_GRADUAL
    struct RClass *cUnLZ4Gradual = mrb_define_class_under(mrb, cBlockDecoder, "Gradual", mrb_cObject);
    MRB_SET_INSTANCE_TT(cUnLZ4Gradual, MRB_TT_DATA);
    mrb_define_method(mrb, cUnLZ4Gradual, "initialize", unlz4g_initialize, MRB_ARGS_ANY());
    mrb_define_method(mrb, cUnLZ4Gradual, "read", unlz4g_read, MRB_ARGS_ANY());
    //mrb_define_method(mrb, cUnLZ4Gradual, "close", unlz4g_close, MRB_ARGS_NONE());
    mrb_define_method(mrb, cUnLZ4Gradual, "maybe_eof", unlz4g_eof, MRB_ARGS_NONE());
#endif /* WITHOUT_UNLZ4_GRADUAL */
}

/*
 * initializer lz4
 * module LZ4
 */

void
mrb_mruby_lz4_gem_init(MRB)
{
    struct RClass *mLZ4 = mrb_define_module(mrb, "LZ4");

#ifndef MRB_INT16
    init_encoder(mrb, mLZ4);
    init_decoder(mrb, mLZ4);
#endif

    init_block_encoder(mrb, mLZ4);
    init_block_decoder(mrb, mLZ4);
}

void
mrb_mruby_lz4_gem_final(MRB)
{
}
