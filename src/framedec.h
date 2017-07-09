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
common_read_args(MRB, mrb_int *size, VALUE *dest)
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
        } else {
            *dest = mrb_str_resize(mrb, *dest, AUX_LZ4_DEFAULT_PARTIAL_SIZE);
        }
    } else {
        if (NIL_P(*dest)) {
            *dest = mrb_str_buf_new(mrb, *size);
        }
    }

    *dest = mrb_str_resize(mrb, *dest, *size);
    RSTR_SET_LEN(RSTRING(*dest), 0);
}

/*
 * call-seq:
 *  read(size = nil, dest = "") -> dest
 */
static VALUE
dec_read(MRB, VALUE self)
{
    struct decoder *p = getdecoder(mrb, self);
    mrb_int size;
    VALUE dest;
    common_read_args(mrb, &size, &dest);
    if (size == 0) { return dest; }

    int arena = mrb_gc_arena_save(mrb);
    size_t srcoff = 0;
    size_t destoff = 0;
    size_t maxdest = size;

    while (size < 0 || destoff < (size_t)size) {
        if (p->inoff >= RSTRING_LEN(p->inbuf)) {
            mrb_gc_arena_restore(mrb, arena);

            if (p->inbufsize < 1) { p->inbufsize = 0; break; }

            VALUE v = FUNCALLC(mrb, p->inport, "read", mrb_fixnum_value(p->inbufsize), p->inbuf);
            if (NIL_P(v)) { p->inbufsize = 0; break; }
            mrb_check_type(mrb, v, MRB_TT_STRING);
            if (RSTRING_LEN(v) < 1) { p->inbufsize = 0; break; }
            if (mrb_obj_eq(mrb, v, p->inbuf)) {
                decoder_set_inbuf(mrb, self, p, v);
            }

            p->inoff = 0;
        }

        const char *srcp = RSTRING_PTR(p->inbuf) + p->inoff;
        size_t srcsize = RSTRING_LEN(p->inbuf) - p->inoff;
        char *destp = RSTRING_PTR(dest) + destoff;
        size_t destsize = RSTRING_CAPA(dest) - destoff;
        size_t s = LZ4F_decompress(p->lz4f, destp, &destsize, srcp, &srcsize, NULL);
        p->inoff += srcsize;
        destoff += destsize;
        RSTR_SET_LEN(RSTRING(dest), destoff);
        aux_lz4f_check_error(mrb, s, "LZ4F_decompress");

        if (s == 0 || LZ4F_isError(s) ||
            maxdest >= AUX_STR_MAX || size >= destoff) {

            break;
        }
        if (size < 0) {
            maxdest += AUX_LZ4_DEFAULT_PARTIAL_SIZE;
            maxdest = CLAMP_MAX(maxdest, AUX_STR_MAX);
            dest = aux_str_resize(mrb, dest, maxdest);
        }
    }

    if (destoff > 0) {
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
