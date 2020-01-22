# Tests are implemented as a series of example C files, each of which should
# compile successfully without Tartan, and which should fail compilation
# with Tartan (and -Werror).

tests = [
    'assertion-extraction.c',
    'assertion-extraction-cpp.cpp',
    'assertion-extraction-return.c',
    'gerror-api.c',
    'gsignal-connect.c',
    'gvariant-builder.c',
    'gvariant-get.c',
    'gvariant-get-child.c',
    'gvariant-iter.c',
    'gvariant-lookup.c',
    'gvariant-new.c',
    'non-glib.c',
    'nonnull.c',
]

test_driver = find_program('driver.py')
target_clang = find_program('clang')

foreach test_name : tests
    test_file = files(test_name)
    test_args = [test_file, '--target-cc=@0@'.format(target_clang.path())]
    test(test_name, test_driver, args: test_args, protocol: 'tap',
        timeout: 1200)
endforeach
