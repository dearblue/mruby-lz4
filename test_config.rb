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

  gem core: "mruby-print"
  gem core: "mruby-bin-mrbc"
  gem core: "mruby-bin-mruby"
  gem "."
end
