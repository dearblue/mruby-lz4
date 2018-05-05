# mruby-lz4 : mruby bindings for lz4 the compression library (unofficial)

mruby へ LZ4 圧縮ライブラリの機能を提供します。

  * LZ4 Frame データの圧縮・伸長が行なえます。

    LZ4::Encoder / LZ4::Decoder

  * LZ4 Block データの圧縮・伸長が行なえます。

    LZ4::BlockEncoder / LZ4::BlockDecoder

## ***注意***

  * LZ4 Frame API は ``MRB_INT16`` が定義された場合利用できません。

  * LZ4 Frame API は mruby のメモリアロケータを通してメモリが確保されません。

    メモリ関連で不可解な挙動があった場合、このことが原因であるかもしれません。


## HOW TO USAGE

### 圧縮 (LZ4 Block Format)

```ruby
src = "123456789"
dest = LZ4.block_encode(src)
```

圧縮レベルを指定したい場合:

```ruby
src = "123456789"
complevel = 10 # 整数値を与える。既定値は -1 で、C の LZ4_compress_fast() と等価
               # 負の値であれば LZ4_compress_fast_continue() の accelerator に絶対値を渡すことを意味する
               # 0 以上の値であれば LZ4_resetStreamHC() の level に値を渡すことを意味する
dest = LZ4.block_encode(src, level: complevel)
```

### 伸長 (LZ4 Block Format)

```ruby
lz4seq = ... # lz4'd string by LZ4.block_encode
dest = LZ4.block_decode(lz4seq)
```

### 圧縮 (LZ4 Frame Format)

```ruby
src = "123456789"
dest = LZ4.encode(src)
```

圧縮レベルを指定したい場合:

```ruby
src = "123456789"
complevel = 15 # 1..22 の範囲で与える。既定値は nil で、1 と等価
dest = LZ4.encode(src, level: complevel)
```

### 伸長 (LZ4 Frame Format)

```ruby
lz4seq = ... # lz4'd string by LZ4.encode
              # OR .lz4 file data by lz4-cli
dest = LZ4.decode(lz4seq)
```

### ストリーミング圧縮 (LZ4 Frame Format)

```ruby
output = AnyObject.new # An object that has ``.<<'' method (e.g. IO, StringIO, or etc.)
LZ4.encode(output) do |lz4|
  lz4 << "abcdefg"
  lz4 << "123456789" * 99
end
```

### ストリーミング伸長 (LZ4 Frame Format)

```ruby
input = AnyObject.new # An object that has ``.read'' method (e.g. IO, StringIO, or etc.)
LZ4.decode(input) do |lz4|
  lz4.read(20)
  buf = ""
  lz4.read(5, buf)
  lz4.read(10, buf)
  lz4.read(nil, buf)
end
```


## Specification

  * Product name: [mruby-lz4](https://github.com/dearblue/mruby-lz4)
  * Version: 0.3
  * Product quality: PROTOTYPE
  * Author: [dearblue](https://github.com/dearblue)
  * Report issue to: <https://github.com/dearblue/mruby-lz4/issues>
  * Licensing: BSD-2-Clause License
  * Dependency external mrbgems:
      * [mruby-aux](https://github.com/dearblue/mruby-aux)
        under [Creative Commons Zero License \(CC0\)](https://github.com/dearblue/mruby-aux/blob/master/LICENSE)
        by [dearblue](https://github.com/dearblue)
  * Bundled C libraries (git-submodules):
      * [lz4-1.8.1.2](https://github.com/lz4/lz4)
        under [BSD-2-Clause License](https://github.com/lz4/lz4/blob/v1.8.1.2/LICENSE)
        by [Yann Collet](https://github.com/Cyan4973)
