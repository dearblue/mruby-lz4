module LZ4
  if const_defined? :Encoder
    #
    # call-seq:
    #   encode(input_string, maxsize = nil, dest = "", opts = {}) -> lz4 compressed string
    #   encode(input_string, dest, opts = {}) -> lz4 compressed string
    #   encode(output_io, opts = {}) -> instance of LZ4::Encoder
    #   encode(output_io, opts = {}) { |instance of LZ4::Encoder| ... } -> yeald value
    #
    # [input_string (String)]
    #   string object
    #
    # [output_io (any object)]
    #   Output port for LZ4 frame stream.
    #   Need +.<<+ method.
    #
    # [prefs (Hash)]
    #   level = nil (nil or 0..9)::
    #   blocksize = nil (nil OR unsigned integer)::
    #   blocklink = true (true OR false)::
    #   checksum = true (true OR false)::
    #
    def LZ4.encode(port, *args, &block)
      if port.is_a?(String)
        LZ4::Encoder.encode(port, *args)
      else
        LZ4::Encoder.wrap(port, *args, &block)
      end
    end

    def LZ4.decode(port, *args, &block)
      if port.is_a?(String)
        LZ4::Decoder.decode(port, *args)
      else
        LZ4::Decoder.wrap(port, *args, &block)
      end
    end

    module StreamWrapper
      def wrap(*args)
        lz4 = new(*args)

        return lz4 unless block_given?

        begin
          yield lz4
        ensure
          lz4.close rescue nil
          lz4 = nil
        end
      end
    end

    Encoder.extend StreamWrapper
    Decoder.extend StreamWrapper

    Compressor = Encoder
    Deompressor = Decoder
    Unompressor = Decoder

    class << LZ4
      alias compress encode
      alias decompress decode
      alias uncompress decode
    end

    class << Encoder
      alias compress encode
    end

    class << Decoder
      alias decompress decode
      alias uncompress decode
    end
  end

  BlockCompressor = BlockEncoder
  BlockDeompressor = BlockDecoder
  BlockUnompressor = BlockDecoder

  class << BlockEncoder
    alias compress encode
    alias compress_size encode_size
    alias encode_bound encode_size
    alias compress_bound encode_size
  end

  class << BlockDecoder
    alias decompress decode
    alias uncompress decode
    alias decompress_size decode_size
    alias uncompress_size decode_size
  end

  def LZ4.block_encode(*args)
    LZ4::BlockEncoder.encode(*args)
  end

  def LZ4.block_decode(*args)
    LZ4::BlockDecoder.decode(*args)
  end

  class << LZ4
    alias block_compress block_encode
    alias block_decompress block_decode
    alias block_uncompress block_decode
  end
end
