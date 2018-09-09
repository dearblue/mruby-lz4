/**
 * @file unlz4-gradual.h
 */

#ifndef UNLZ4_GRADUAL_H
#define UNLZ4_GRADUAL_H 1

#ifdef __cplusplus
# define UNLZ4_GRADUAL_C_DECL       extern "C"
# define UNLZ4_GRADUAL_C_DECL_BEGIN UNLZ4_GRADUAL_C_DECL {
# define UNLZ4_GRADUAL_C_DECL_END   }
#else
# define UNLZ4_GRADUAL_C_DECL
# define UNLZ4_GRADUAL_C_DECL_BEGIN
# define UNLZ4_GRADUAL_C_DECL_END
#endif

#if defined(NO_MALLOC) || defined(_NO_MALLOC) || defined(__NO_MALLOC__)
# undef UNLZ4_GRADUAL_NO_MALLOC
# define UNLZ4_GRADUAL_NO_MALLOC 1
#endif

UNLZ4_GRADUAL_C_DECL_BEGIN

#include <stdint.h>
#include <stddef.h>

#define UNLZ4_GRADUAL_MAX_PREFIX_LENGTH 65536L

struct unlz4_gradual
{
    const char *next_in;
    int32_t avail_in;
    int32_t total_in;

    char *next_out;
    int32_t avail_out;
    int32_t total_out;

    void *opaque; /* 利用者定義のデータ */
};

enum unlz4_gradual_status
{
    /** 正常に完了しました。 */
    UNLZ4_GRADUAL_OK = 0,

    /** 内部状態はちょうど終了位置にあります。これ以上入力がないのであれば正常に完了しました。 */
    UNLZ4_GRADUAL_MAYBE_FINISHED = -1,

    /** 内部状態はさらなる入力を必要としています。next_in と avail_in を正しく設定して下さい。 */
    UNLZ4_GRADUAL_NEED_INPUT = -2,

    /** 内部状態は出力するべきものが残っています。next_out と avail_out を正しく設定して下さい。 */
    UNLZ4_GRADUAL_NEED_OUTPUT = -3,

    /** メモリの確保に失敗しました。主な原因はメモリ不足か、リソース制限に達したためです。 */
    UNLZ4_GRADUAL_ERROR_NO_MEMORY = 1,

    /** lz4 シーケンスの offset が prefix buffer を超えたため続行できません。 */
    UNLZ4_GRADUAL_ERROR_OUT_OF_PREFIX_BUFFER = 2,

    /** 内部バグです。作者に報告して下さい。 */
    UNLZ4_GRADUAL_ERROR_UNEXPECT_REACHED_HERE = 99,
};

/**
 * unlz4_gradual コンテキストを生成して初期化します。
 *
 * 引数 alloc にメモリアロケータとしての関数を渡すと、メモリの確保のために利用者定義の関数を用いることが出来ます。
 *
 * NULL であれば標準 C ライブラリの malloc 関数が使われます。
 *
 * 生成されたコンテキストは引数 p で与えられたポインタ変数に渡されます。
 *
 * 辞書を設定するのであれば、続けて unlz4_gradual_reset() を呼んで下さい。
 *
 * 辞書を必要としないのであればこのまま unlz4_gradual() を呼ぶことが出来ます。
 *
 * このコンテキストが不要になったら、alloc によるメモリアロケータと対となる関数を使って開放して下さい。
 * alloc が NULL であれば、標準 C ライブラリの free() を呼ぶだけです。
 *
 * 成功した場合、UNLZ4_GRADUAL_OK を返します。
 * 失敗すれば enum unlz4_gradual_status で定義されたそれ以外を返します。
 */
extern enum unlz4_gradual_status unlz4_gradual_alloc(struct unlz4_gradual **p, int32_t prefix_capacity, void *alloc(void *user, size_t), void *user);

/**
 * 引数 p で示された unlz4_gradual コンテキストを初期状態に戻します。
 *
 * 引数 prefix が NULL で、引数 prefixlen が負の整数である場合、コンテキストの prefix はそのままで初期状態にします。
 *
 * 引数 prefix が NULL で、引数 prefixlen が0以上の整数であれば、コンテキストの prefix も初期化されます。
 *
 * 引数 prefix が非 NULL であれば、引数 prefixlen の長さと共にコンテキストの prefix を初期化します。
 *
 * 成功した場合、UNLZ4_GRADUAL_OK を返します。
 * 失敗すれば enum unlz4_gradual_status で定義されたそれ以外を返します。
 */
extern enum unlz4_gradual_status unlz4_gradual_reset(struct unlz4_gradual *p, const void *prefix, int32_t prefixlen);

/**
 * next_in で示されたポインタアドレスに続く lz4 ブロックデータを伸長し、next_out に書き込んでいきます。zlib を参考にしています。
 *
 * 成功した場合、UNLZ4_GRADUAL_MAYBE_FINISHED / UNLZ4_GRADUAL_NEED_INPUT / UNLZ4_GRADUAL_NEED_OUTPUT のいずれかを返します。
 * 失敗すれば enum unlz4_gradual_status で定義されたそれ以外を返します。
 *
 * いかなる場合でも UNLZ4_GRADUAL_OK が返ることはありません。
 */
extern enum unlz4_gradual_status unlz4_gradual(struct unlz4_gradual *p);

/**
 * enum unlz4_gradual_status に対応した文字列を返します。
 */
extern const char *unlz4_gradual_str_status(enum unlz4_gradual_status s);

UNLZ4_GRADUAL_C_DECL_END

#endif /* UNLZ4_GRADUAL_H */
