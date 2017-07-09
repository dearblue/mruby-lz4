#!ruby

assert("LZ4 Block API - one step processing") do
  s = "123456789" * 1111

  assert_equal s, LZ4.block_decode(LZ4.block_encode(s))
  assert_equal s, LZ4.block_decode(LZ4.block_encode(s, ""))
  assert_equal s, LZ4.block_decode(LZ4.block_encode(s), "")

  d = ""
  assert_equal d.object_id, LZ4.block_encode(s, d).object_id
  #d = ""
  assert_equal d.object_id, LZ4.block_decode(LZ4.block_encode(s), d).object_id

  # 与える level が大き過ぎたり小さ過ぎたりしても受け入れられる
  # 範囲外の値については lz4 ライブラリが良きに計らってくれるため
  assert_equal s, LZ4.block_decode(LZ4.block_encode(s, level: -100))
  assert_equal s, LZ4.block_decode(LZ4.block_encode(s, level: 100))

  # predict で辞書を用いた処理を行える
  assert_equal s, LZ4.block_decode(LZ4.block_encode(s, predict: "123456789123456789"), predict: "123456789123456789")
  # predict を用いて圧縮したデータは、伸長時にも必要
  assert_raise(RuntimeError) { LZ4.block_decode(LZ4.block_encode(s, predict: "123456789123456789")) }
end
