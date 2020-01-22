plugin_sources = [
    'assertion-extracter.cpp',
    'assertion-extracter.h',
    'checker.cpp',
    'checker.h',
    'debug.cpp',
    'debug.h',
    'gassert-attributes.cpp',
    'gassert-attributes.h',
    'gerror-checker.cpp',
    'gerror-checker.h',
    'gir-attributes.cpp',
    'gir-attributes.h',
    'gir-manager.cpp',
    'gir-manager.h',
    'gsignal-checker.cpp',
    'gsignal-checker.h',
    'gvariant-checker.cpp',
    'gvariant-checker.h',
    'nullability-checker.cpp',
    'nullability-checker.h',
    'plugin.cpp',
    'type-manager.cpp',
    'type-manager.h',
]

version_arr = llvm.version().split('.')
llvm_major = version_arr[0]
llvm_minor = version_arr[1]
normalized_llvm_version = '@0@.@1@'.format(llvm_major, llvm_minor)

plugindir = join_paths(get_option('libdir'), meson.project_name(),
    normalized_llvm_version)

plugin = shared_module('tartan', plugin_sources, name_suffix: 'so',
    dependencies: [llvm, glib, gobject, gio, gi],
    cpp_args: ['-DG_LOG_DOMAIN="tartan"'],
    include_directories: config_h_include,
    install: true, install_dir: plugindir)
