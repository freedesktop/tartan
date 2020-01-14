project('tartan', 'cpp', version: '0.4.0', license: 'GPL3',
    meson_version: '>= 0.52',
    default_options: ['cpp_std=c++11', 'warning_level=3'])

cxx = meson.get_compiler('cpp')

### Check for required libraries ###############################################

llvm_requirement = '>= 7.0'
glib_requirement = '>= 2.38'
gir_requirement = '>= 1.38.0'

glib = dependency('glib-2.0', version: glib_requirement,
    fallback: ['glib', 'libglib_dep'])
gobject = dependency('gobject-2.0', version: glib_requirement,
    fallback: ['glib', 'libgobject_dep'])
gio = dependency('gio-2.0', version: glib_requirement,
    fallback: ['glib', 'libgio_dep'])
gi = dependency('gobject-introspection-1.0', version: gir_requirement,
    fallback: ['gobject-introspection', 'girepo_dep'])

llvm_with_link = dependency('llvm', version: llvm_requirement,
    include_type: 'system')
# Don't link against the LLVM libs because they're huge. All the symbols
# will be available when the plugin is loaded anyway.
llvm = llvm_with_link.partial_dependency(compile_args: true)

cxx.check_header('clang/AST/Expr.h', dependencies: llvm, required: true)

### Generate config.h ##########################################################

header_conf = configuration_data()

header_conf.set_quoted('VERSION', meson.project_version())
header_conf.set_quoted('LLVM_CONFIG_VERSION', llvm.version())
header_conf.set('HAVE_LLVM_8_0', llvm.version().version_compare('>= 8.0'))
header_conf.set('HAVE_LLVM_9_0', llvm.version().version_compare('>= 9.0'))
header_conf.set('GETTEXT_PACKAGE', meson.project_name())

configure_file(output: 'config.h', configuration: header_conf)
config_h_include = include_directories('.')

### Build ######################################################################

subdir('po')
subdir('clang-plugin')
subdir('scripts')
subdir('tests')
