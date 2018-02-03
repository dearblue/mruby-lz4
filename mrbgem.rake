MRuby::Gem::Specification.new("mruby-lz4") do |s|
  s.summary = "mruby bindings for lz4 the compression library (unofficial)"
  s.version = "0.2"
  s.license = "BSD-2-Clause"
  s.author  = "dearblue"
  s.homepage = "https://github.com/dearblue/mruby-lz4"

  add_dependency "mruby-string-ext", core: "mruby-string-ext"
  add_dependency "mruby-aux", github: "dearblue/mruby-aux"
  add_dependency "mruby-aux-scanhash", github: "dearblue/mruby-aux-scanhash"

  if s.cc.command =~ /\b(?:g?cc|clang)\d*\b/
    s.cc.flags << "-Wall" <<
                  "-Wno-shift-negative-value" <<
                  "-Wno-shift-count-negative" <<
                  "-Wno-shift-count-overflow" <<
                  "-Wno-missing-braces"
  end

  unless File.exist?(File.join(dir, "contrib/lz4/lib"))
    Dir.chdir dir do
      system "git submodule init" or fail
      system "git submodule update" or fail
    end
  end if false

  dirp = dir.gsub(/[\[\]\{\}\,]/) { |m| "\\#{m}" }
  files = "contrib/lz4/lib/**/*.c"
  objs.concat(Dir.glob(File.join(dirp, files)).map { |f|
    next nil unless File.file? f
    objfile f.relative_path_from(dir).pathmap("#{build_dir}/%X")
  }.compact)

  cc.include_paths.insert 0, File.join(dir, "contrib/lz4/lib")
end
