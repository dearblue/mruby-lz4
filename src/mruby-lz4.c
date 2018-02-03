#include <mruby.h>
#include <mruby/class.h>
#include <mruby/hash.h>
#include <mruby/string.h>
#include <mruby/value.h>
#include <mruby/data.h>
#include <mruby/variable.h>
#include <lz4.h>
#include <lz4hc.h>
#include <lz4frame.h>
#include <mruby-aux.h>
#include <mruby-aux/scanhash.h>
#include <string.h>

#define AUX_LZ4_DEFAULT_PARTIAL_SIZE ((uint32_t)256 << 10)

#define AUX_OR_DEFAULT(primary, secondary) (NIL_P(primary) ? (secondary) : (primary))
#define CLAMP(n, min, max) (n < min ? min : (n > max ? max : n))
#define CLAMP_MAX(n, max) (n > max ? max : n)
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

static inline VALUE
aux_str_set_len(VALUE str, mrb_int size)
{
    RSTR_SET_LEN(RSTRING(str), size);
    return str;
}

static inline VALUE
aux_str_buf_new(MRB, size_t size)
{
    return mrb_str_buf_new(mrb, CLAMP_MAX(size, AUX_STR_MAX));
}

static inline VALUE
aux_str_resize(MRB, VALUE str, size_t size)
{
    size_t len = RSTRING_LEN(str);
    mrb_str_resize(mrb, str, CLAMP_MAX(size, AUX_STR_MAX));
    aux_str_set_len(str, len);
    return str;
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
    aux_str_resize(mrb, str, size);

    return str;
}

static int
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

    maxsize = CLAMP_MAX(maxsize, AUX_STR_MAX);

    if (NIL_P(*dest)) {
        *dest = aux_str_buf_new(mrb, maxsize);
    } else {
        mrb_check_type(mrb, *dest, MRB_TT_STRING);
        aux_str_resize(mrb, *dest, maxsize);
        aux_str_set_len(*dest, 0);
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
    aux_str_set_len(dest, s);
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
    struct RData *rd;
    struct encoder *p;
    Data_Make_Struct(mrb, mrb_class_ptr(self), struct encoder, &encoder_type, p, rd);
    LZ4F_errorCode_t err = LZ4F_createCompressionContext(&p->lz4f, LZ4F_getVersion());
    aux_lz4f_check_error(mrb, err, "LZ4F_createCompressionContext");
    p->io = Qnil;
    p->outbuf = Qnil;
    p->outbufsize = CLAMP_MAX(256 << 10, AUX_STR_MAX); /* AUX_STR_MAX or 256 KiB */

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
    aux_str_set_len(p->outbuf, s);
    FUNCALL(mrb, p->io, SYMBOL("<<"), p->outbuf);

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
        size_t insize = CLAMP_MAX(srclen, 4 * 1024 * 1024);
        size_t outsize = LZ4F_compressBound(insize, &p->prefs);
        encoder_set_outbuf(mrb, self, p, aux_str_alloc(mrb, p->outbuf, outsize));
        char *dest = RSTRING_PTR(p->outbuf);
        size_t s = LZ4F_compressUpdate(p->lz4f, dest, outsize, src, insize, &opts);
        aux_lz4f_check_error(mrb, s, "LZ4F_compressUpdate");
        aux_str_set_len(p->outbuf, s);
        FUNCALL(mrb, p->io, SYMBOL("<<"), p->outbuf);
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
    aux_str_set_len(p->outbuf, s);
    FUNCALL(mrb, p->io, SYMBOL("<<"), p->outbuf);

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
    aux_str_set_len(p->outbuf, s);
    FUNCALL(mrb, p->io, SYMBOL("<<"), p->outbuf);

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
    LZ4F_decompressOptions_t opts = { .stableDst = 1, };
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

/*
 * call-seq:
 *  decode(src, destsize = nil, dest = "") -> dest
 */
static VALUE
dec_s_decode(MRB, VALUE self)
{
    struct RString *src, *dest;
    ssize_t maxdest;
    dec_s_decode_args(mrb, &src, &dest, &maxdest);
    LZ4F_dctx *context;
    size_t s;

    s = LZ4F_createDecompressionContext(&context, LZ4F_VERSION);
    aux_lz4f_check_error(mrb, s, "LZ4F_createDecompressionContext");

    if (maxdest < 0) {
        dec_s_decode_all(mrb, self, src, dest, context);
    } else {
        dec_s_decode_partial(mrb, self, src, dest, maxdest, context);
    }

    LZ4F_freeDecompressionContext(context);

    return VALUE(dest);
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
    struct RData *rd;
    struct decoder *p;
    Data_Make_Struct(mrb, mrb_class_ptr(self), struct decoder, &decoder_type, p, rd);

    LZ4F_errorCode_t err = LZ4F_createDecompressionContext(&p->lz4f, LZ4F_VERSION);
    aux_lz4f_check_error(mrb, err, "LZ4F_createDecompressionContext");
    p->inport = Qnil;
    p->inbuf = Qnil;
    p->inbufsize = CLAMP_MAX(1 << 20, AUX_STR_MAX); /* AUX_STR_MAX or 1 MiB */

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

static void
common_read_args(MRB, intptr_t *size, VALUE *dest)
{
    VALUE asize = Qnil;
    switch (mrb_get_args(mrb, "|oS!", &asize, dest)) {
    case 0:
        *size = -1;
        *dest = Qnil;
        break;
    case 1:
        *size = (NIL_P(asize) ? -1 : mrb_int(mrb, asize));
        *dest = Qnil;
        break;
    case 2:
        *size = (NIL_P(asize) ? -1 : mrb_int(mrb, asize));
        break;
    default:
        AUX_NOT_REACHED_HERE;
    }

    if (*size < 0) {
        if (NIL_P(*dest)) {
            *dest = mrb_str_buf_new(mrb, AUX_LZ4_DEFAULT_PARTIAL_SIZE);
        }
        mrb_str_resize(mrb, *dest, AUX_LZ4_DEFAULT_PARTIAL_SIZE);
    } else {
        if (NIL_P(*dest)) {
            *dest = mrb_str_buf_new(mrb, *size);
        }
        mrb_str_resize(mrb, *dest, *size);
    }

    RSTR_SET_LEN(RSTRING(*dest), 0);
}

static int
dec_read_fetch(MRB, VALUE self, struct decoder *p)
{
    if (p->inoff >= RSTRING_LEN(p->inbuf)) {
        if (p->inbufsize < 1) { p->inbufsize = 0; return -1; }

        VALUE v = FUNCALL(mrb, p->inport, SYMBOL("read"), mrb_fixnum_value(p->inbufsize), p->inbuf);
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
    VALUE dest;
    common_read_args(mrb, &size, &dest);
    if (size == 0) { return dest; }

    int arena = mrb_gc_arena_save(mrb);

    while (size < 0 || RSTRING_LEN(dest) < size) {
        mrb_gc_arena_restore(mrb, arena);

        if (dec_read_fetch(mrb, self, p) < 0) {
            break;
        }

        const char *srcp = RSTRING_PTR(p->inbuf) + p->inoff;
        size_t srcsize = RSTRING_LEN(p->inbuf) - p->inoff;
        char *destp = RSTRING_PTR(dest) + RSTRING_LEN(dest);
        size_t destsize = (size < 0 ? RSTRING_CAPA(dest) : size) - RSTRING_LEN(dest);
        size_t s = LZ4F_decompress(p->lz4f, destp, &destsize, srcp, &srcsize, NULL);
        p->inoff += srcsize;
        RSTR_SET_LEN(RSTRING(dest), RSTRING_LEN(dest) + destsize);
        aux_lz4f_check_error(mrb, s, "LZ4F_decompress");
        if (s == 0 || RSTRING_LEN(dest) >= AUX_STR_MAX) {
            break;
        }

        if (size < 0 && RSTRING_LEN(dest) >= RSTRING_CAPA(dest)) {
            size_t capa = RSTRING_CAPA(dest) + AUX_LZ4_DEFAULT_PARTIAL_SIZE;
            capa = CLAMP_MAX(capa, AUX_STR_MAX);
            aux_str_resize(mrb, dest, capa);
        }
    }

    if (RSTRING_LEN(dest) > 0) {
        return dest;
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
blkenc_s_encode_args(MRB, VALUE *src, VALUE *dest, int *level, VALUE *predict)
{
    mrb_int argc;
    VALUE *argv;
    mrb_get_args(mrb, "*", &argv, &argc);
    if (argc > 0 && mrb_hash_p(argv[argc - 1])) {
        VALUE alevel;
        MRBX_SCANHASH(mrb, argv[argc - 1], Qnil,
                MRBX_SCANHASH_ARGS("level", &alevel, Qnil),
                MRBX_SCANHASH_ARGS("predict", predict, Qnil));

        *level = (NIL_P(alevel) ? -1 : mrb_int(mrb, alevel));

        if (!NIL_P(*predict)) {
            mrb_check_type(mrb, *predict, MRB_TT_STRING);
        }

        argc --;
    } else {
        *level = -1;
        *predict = Qnil;
    }

    size_t maxdest;
    switch (argc) {
    case 1:
        *src = argv[0];
        maxdest = -1;
        *dest = Qnil;
        break;
    case 2:
        *src = argv[0];
        if (mrb_string_p(argv[1])) {
            maxdest = -1;
            *dest = argv[1];
        } else {
            maxdest = aux_to_u32(mrb, argv[1]);
            *dest = Qnil;
        }
        break;
    case 3:
        *src = argv[0];
        maxdest = aux_to_u32(mrb, argv[1]);
        *dest = argv[2];
        break;
    default:
        mrb_raisef(mrb,
                   E_ARGUMENT_ERROR,
                   "wrong number of arguments (given %S, expect 1..4 + keywords)",
                   mrb_fixnum_value(argc));
    }

    mrb_check_type(mrb, *src, MRB_TT_STRING);

    if (maxdest == -1) {
        maxdest = LZ4_compressBound(RSTRING_LEN(*src));
    }
    maxdest = CLAMP_MAX(maxdest, AUX_STR_MAX);

    if (NIL_P(*dest)) {
        *dest = aux_str_buf_new(mrb, maxdest);
    } else {
        mrb_check_type(mrb, *dest, MRB_TT_STRING);
    }
    aux_str_resize(mrb, *dest, maxdest);
    aux_str_set_len(*dest, 0);
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
    VALUE src, dest, predict;
    int level;
    blkenc_s_encode_args(mrb, &src, &dest, &level, &predict);

    if (level < 0) {
        int s;
        LZ4_stream_t *lz4 = (LZ4_stream_t *)mrb_malloc(mrb, sizeof(LZ4_stream_t));
        LZ4_resetStream(lz4);
        if (!NIL_P(predict)) {
            s = LZ4_loadDict(lz4, RSTRING_PTR(predict), RSTRING_LEN(predict));
        }
        s = LZ4_compress_fast_continue(lz4, RSTRING_PTR(src), RSTRING_PTR(dest), RSTRING_LEN(src), RSTRING_CAPA(dest), -level);
        mrb_free(mrb, lz4);
        if (s <= 0) { mrb_raisef(mrb, E_RUNTIME_ERROR, "LZ4_compress_fast_continue failed (code:%S)", mrb_fixnum_value(s)); }
        aux_str_set_len(dest, s);
    } else {
        int s;
        LZ4_streamHC_t *lz4 = (LZ4_streamHC_t *)mrb_malloc(mrb, sizeof(LZ4_streamHC_t));
        LZ4_resetStreamHC(lz4, level);
        if (!NIL_P(predict)) {
            s = LZ4_loadDictHC(lz4, RSTRING_PTR(predict), RSTRING_LEN(predict));
        }
        s = LZ4_compress_HC_continue(lz4, RSTRING_PTR(src), RSTRING_PTR(dest), RSTRING_LEN(src), RSTRING_CAPA(dest));
        mrb_free(mrb, lz4);
        if (s <= 0) { mrb_raisef(mrb, E_RUNTIME_ERROR, "LZ4_compress_HC_continue failed (code:%S)", mrb_fixnum_value(s)); }
        aux_str_set_len(dest, s);
    }

    return dest;
}

static void
init_block_encoder(MRB, struct RClass *mLZ4)
{
    struct RClass *cBlockEncoder = mrb_define_class_under(mrb, mLZ4, "BlockEncoder", mrb_cObject);
    mrb_define_class_method(mrb, cBlockEncoder, "encode_size", blkenc_s_encode_size, MRB_ARGS_REQ(1));
    mrb_define_class_method(mrb, cBlockEncoder, "encode", blkenc_s_encode, MRB_ARGS_ANY());

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
    maxdest = CLAMP_MAX((int64_t)maxdest, (int64_t)AUX_STR_MAX);

    if (NIL_P(*dest)) {
        *dest = aux_str_buf_new(mrb, maxdest);
    } else {
        mrb_check_type(mrb, *dest, MRB_TT_STRING);
    }
    aux_str_resize(mrb, *dest, maxdest);
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
    aux_str_set_len(dest, s);

    return dest;
}

static void
init_block_decoder(MRB, struct RClass *mLZ4)
{
    struct RClass *cBlockDecoder = mrb_define_class_under(mrb, mLZ4, "BlockDecoder", mrb_cObject);
    mrb_define_class_method(mrb, cBlockDecoder, "decode_size", blkdec_s_decode_size, MRB_ARGS_REQ(1));
    mrb_define_class_method(mrb, cBlockDecoder, "decode", blkdec_s_decode, MRB_ARGS_ANY());
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
