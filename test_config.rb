MRuby::Lockfile.disable rescue nil

MRuby::Build.new("host", "build") do |conf|
  toolchain :clang

  enable_debug
  enable_test

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

  cc.flags << "-std=c++11"
  cxx.flags << "-std=c++11"

  gem core: "mruby-print"
  gem core: "mruby-bin-mrbc"
  gem core: "mruby-bin-mruby"
  gem "."
end
