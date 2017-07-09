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
