MRuby::Build.new do |conf|
  toolchain :clang

  conf.build_dir = "host32"

  enable_debug
  enable_test

  gem core: "mruby-print"
  gem core: "mruby-bin-mirb"
  gem core: "mruby-bin-mruby"
  gem "."
end

MRuby::Build.new("host-nan32") do |conf|
  toolchain :clang

  conf.build_dir = conf.name

  cc.defines << %w(MRB_NAN_BOXING)

  enable_debug
  enable_test

  gem core: "mruby-print"
  gem core: "mruby-bin-mrbc"
  gem core: "mruby-bin-mruby"
  gem "."
end

MRuby::Build.new("host32++") do |conf|
  toolchain :clang

  conf.build_dir = conf.name

  enable_debug
  enable_test
  enable_cxx_abi

  gem core: "mruby-print"
  gem core: "mruby-bin-mrbc"
  gem core: "mruby-bin-mruby"
  gem "."
end

MRuby::Build.new("host16") do |conf|
  toolchain :clang

  conf.build_dir = conf.name

  enable_test

  cc.defines = %w(MRB_INT16)
  cc.flags << "-Wall" << "-O0" << "-std=c11" << "-Wno-declaration-after-statement"
  #cc.command = "gcc-7"
  #cxx.command = "g++-7"

  gem core: "mruby-print"
  gem core: "mruby-bin-mrbc"
  gem "."
end
