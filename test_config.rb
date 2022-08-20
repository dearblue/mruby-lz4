MRuby::Lockfile.disable rescue nil

unless MRuby::Build.method_defined? :defines
  module MRuby
    class Build
      def defines
        conf = self
        proxy = Object.new
        proxy.define_singleton_method(:<<, &->(*defs) {
          conf.compilers.each { |cc| cc.defines << defs }
          proxy
        })
        proxy
      end
    end
  end
end

MRuby::Build.new("host", "build") do |conf|
  toolchain :clang

  enable_debug
  enable_test

  defines << %w(MRB_STR_LENGTH_MAX='(16UL<<20)')

  gem core: "mruby-print"
  gem core: "mruby-bin-mirb"
  gem core: "mruby-bin-mruby"
  gem "."
end

MRuby::Build.new("host++", "build") do |conf|
  toolchain :clang

  enable_debug
  enable_test
  enable_cxx_abi

  defines << %w(MRB_STR_LENGTH_MAX='(16UL<<20)')

  cc.flags << "-std=c++11"
  cxx.flags << "-std=c++11"

  gem core: "mruby-print"
  gem core: "mruby-bin-mrbc"
  gem core: "mruby-bin-mruby"
  gem "."
end
