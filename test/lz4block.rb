#!ruby

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
