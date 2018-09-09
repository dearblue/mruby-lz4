#!ruby

is_mrb16 = (1 << 24).kind_of?(Float)

az104 = "abcdefghijklmnopqrstuvwxyz" * 4

# "abcdefghijklmnopqrstuvwxyz" の巡回文字列104バイトを LZ4 ブロックフォーマットで圧縮したデータ。
az104_lz4 =
  "\xff\x0b\x61\x62\x63\x64\x65\x66\x67\x68\x69\x6a\x6b\x6c\x6d\x6e" <<
  "\x6f\x70\x71\x72\x73\x74\x75\x76\x77\x78\x79\x7a\x1a\x00\x36\x50" <<
  "\x76\x77\x78\x79\x7a"

# "abcdefghijklmnopqrstuvwxyz" の巡回文字列104バイトを LZ4 ブロックフォーマットで圧縮したデータ。
# ただし "abcdefghijklmnopqrstuvwxyz" の辞書が必要。
az104_lz4_linked = "\x0f\x1a\x00\x50\x50\x76\x77\x78\x79\x7a"

# "abcdefghijklmnopqrstuvwxyz" の巡回文字列8346199バイトを LZ4 ブロックフォーマットで三重圧縮したデータ。
# データサイズは 8346199 => 32766 => 172 => 51 バイト。
#
# 途中で 32766 バイトとなるようにしたのは、MRB_INT16 の都合に合わせている。
#
#   ruby -rextlz4 -e 'class String; def lz4(*args); LZ4.block_encode(self, *args); end; end; src = ("abcdefghijklmnopqrstuvwxyz" * (8346226/26)).slice(0, 8346199); lz4s = src.lz4.lz4.lz4; puts lz4s.unpack("H*")[0].gsub(/(?=(?:\w{2})+$)/, "\\\\x").scan(/.{1,64}/)'
az8346199_lz4_lz4_lz4 =
  "\xff\x15\xff\x10\xff\x0b\x61\x62\x63\x64\x65\x66\x67\x68\x69\x6a" <<
  "\x6b\x6c\x6d\x6e\x6f\x70\x71\x72\x73\x74\x75\x76\x77\x78\x79\x7a" <<
  "\x1a\x00\xff\x01\x00\xff\x01\x00\x6c\x90\x45\x70\xfe\x50\x6d\x6e" <<
  "\x6f\x70\x71"

assert("LZ4 Block API - LZ4::BlockEncoder.encode") do
  assert_equal "\0", LZ4::BlockEncoder.encode("")
  assert_equal "\0", LZ4::BlockEncoder.encode("", nil)
  assert_equal "\0", LZ4::BlockEncoder.encode("", "")
  assert_equal "\0", LZ4::BlockEncoder.encode("", 1)
  assert_equal "\0", LZ4::BlockEncoder.encode("", 1, nil)
  assert_equal "\0", LZ4::BlockEncoder.encode("", 1, "")
  assert_equal "\0", LZ4::BlockEncoder.encode("", nil, level: nil)
  assert_equal "\0", LZ4::BlockEncoder.encode("", 1, level: nil)
  assert_equal "\0", LZ4::BlockEncoder.encode("", 1, nil, level: nil)

  assert_equal "\0", LZ4::BlockEncoder.encode("", level: nil)
  assert_equal "\0", LZ4::BlockEncoder.encode("", level: 1)
  assert_equal "\0", LZ4::BlockEncoder.encode("", level: 0)
  assert_equal "\0", LZ4::BlockEncoder.encode("", level: -1)

  assert_equal "\x10A", LZ4::BlockEncoder.encode("A")

  dest = ""
  assert_equal dest.object_id, LZ4::BlockEncoder.encode("A", dest).object_id
  assert_equal dest.object_id, LZ4::BlockEncoder.encode("A", 2, dest).object_id

  # 与える level が大き過ぎたり小さ過ぎたりしても受け入れられる
  # 範囲外の値については lz4 ライブラリが良きに計らってくれるため
  assert_equal "\0", LZ4::BlockEncoder.encode("", level: -100)
  assert_equal "\0", LZ4::BlockEncoder.encode("", level: 100)

  assert_equal "\0", LZ4::BlockEncoder.encode("", predict: "123456789abcdefghijklmnopqrstuvwxyz")
  assert_equal "\0", LZ4::BlockEncoder.encode("", predict: "123456789abcdefghijklmnopqrstuvwxyz", level: 10)

  assert_raise(ArgumentError) { LZ4::BlockEncoder.encode() }
  assert_raise(TypeError) { LZ4::BlockEncoder.encode(nil) }
  assert_raise(TypeError) { LZ4::BlockEncoder.encode("", 1, 2) }
  assert_raise(ArgumentError) { LZ4::BlockEncoder.encode("", 1, 2, 3) }
  assert_raise(ArgumentError) { LZ4::BlockEncoder.encode("", wrong_keyword: nil) }
  assert_raise(ArgumentError) { LZ4::BlockEncoder.encode("", level: 10, wrong_keyword: nil) }
  assert_raise(TypeError) { LZ4::BlockEncoder.encode("", level: ->{}) }
  assert_raise(TypeError) { LZ4::BlockEncoder.encode("", predict: [1, 2, 3, 4]) }
  assert_raise(RuntimeError) { LZ4::BlockEncoder.encode("A", 0) }
end

assert("LZ4 Block API - one step processing") do
  s = "123456789" * 1111

  assert_equal s, LZ4.block_decode(LZ4.block_encode(s))
  assert_equal s, LZ4.block_decode(LZ4.block_encode(s, ""))
  assert_equal s, LZ4.block_decode(LZ4.block_encode(s), "")

  d = ""
  assert_equal d.object_id, LZ4.block_encode(s, d).object_id
  #d = ""
  assert_equal d.object_id, LZ4.block_decode(LZ4.block_encode(s), d).object_id

  # predict で辞書を用いた処理を行える
  assert_equal s, LZ4.block_decode(LZ4.block_encode(s, predict: "123456789123456789"), predict: "123456789123456789")
  # predict を用いて圧縮したデータは、伸長時にも必要
  assert_raise(RuntimeError) { LZ4.block_decode(LZ4.block_encode(s, predict: "123456789123456789")) }
end

assert "streaming LZ4 Block decode" do
  lz4 = LZ4::BlockDecoder.new
  assert_equal az104, lz4.decode(az104_lz4)
  assert_equal az104, lz4.decode(az104_lz4_linked)

  unless is_mrb16
    tmp = "abcdefghijklmnopqrstuvwxyz" * (8346199 / 26)
    tmp << tmp.slice(0, 8346199 - tmp.bytesize)
    assert_equal tmp.hash, lz4.decode(lz4.decode(lz4.decode(az8346199_lz4_lz4_lz4))).hash
  end
end

assert "streaming LZ4 Block encode" do
  lz4 = LZ4::BlockEncoder.new
  assert_equal az104, LZ4.block_decode(lz4.encode(az104))
  assert_equal az104, LZ4.block_decode(lz4.encode(az104), predict: az104)
end
