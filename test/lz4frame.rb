#!ruby

if LZ4.const_defined? :Encoder

assert("LZ4 Frame API - one step processing (simply)") do
  s = "123456789" * 1111
  assert_equal s, LZ4::Decoder.decode(LZ4::Encoder.encode(s))
  assert_equal s, LZ4.decode(LZ4.encode(s))
end

assert("LZ4 Frame API - one step processing (with arguments)") do
  s = "123456789" * 1111
  assert_equal s, LZ4::Decoder.decode(LZ4::Encoder.encode(s, level: 9))
  assert_equal s, LZ4.decode(LZ4.encode(s, level: 9))
  assert_raise(ArgumentError) { LZ4.encode(s, unknown_keyword: 123) }
  assert_raise(TypeError) { LZ4.encode(s, Object.new, Object.new) }
end

assert("LZ4 Frame API - stream processing") do
  s = "123456789" * 111 + "ABCDEFG"
  d = ""
  LZ4::Encoder.wrap(d) do |lz4|
    # expected String
    assert_raise(TypeError) { lz4.write 123456789 }

    assert_equal lz4, lz4.write(s.byteslice(0, 50))
    assert_equal lz4, lz4.write(s.byteslice(50 ... -20))
    assert_equal lz4, lz4.write(s.byteslice(-20, 20))
  end

  assert_equal s, LZ4.decode(d)

  LZ4::Decoder.wrap(d) do |lz4|
    assert_equal s.byteslice(0, 33), lz4.read(33)
    assert_equal s.byteslice(33, 33), lz4.read(33)
    assert_equal s.byteslice(66, 33), lz4.read(33)
    assert_equal s.byteslice(99 .. -1), lz4.read
  end
end

assert("LZ4 Frame API - stream processing (huge)") do
  s = "123456789" * 1111111 + "ABCDEFG"
  d = ""
  LZ4::Encoder.wrap(d, level: 1) do |lz4|
    off = 0
    slicesize = 777777
    while off < s.bytesize
      assert_equal lz4, lz4.write(s.byteslice(off, slicesize))
      off += slicesize
      slicesize = slicesize * 3 + 7
    end
  end

  assert_equal s.hash, LZ4.decode(d, s.bytesize).hash
  assert_equal s.hash, LZ4.decode(d).hash

  LZ4::Decoder.wrap(d) do |lz4|
    off = 0
    slicesize = 3
    while off < s.bytesize
      assert_equal s.byteslice(off, slicesize).hash, lz4.read(slicesize).hash
      off += slicesize
      slicesize = slicesize * 2 + 3
    end

    assert_equal nil.hash, lz4.read(slicesize).hash
  end
end

end # LZ4::Encoder defined
