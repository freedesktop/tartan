# Tests are implemented as a series of example C files, each of which should
# compile successfully without Tartan, and which should fail compilation
# with Tartan (and -Werror).

TEST_EXTENSIONS = .c .cpp
AM_TESTS_ENVIRONMENT = export abs_top_builddir=$(abs_top_builddir);
C_LOG_COMPILER = $(top_srcdir)/tests/wrapper-compiler-errors
CPP_LOG_COMPILER = $(C_LOG_COMPILER)

c_tests = \
	assertion-extraction.c \
	assertion-extraction-cpp.cpp \
	assertion-extraction-return.c \
	gsignal-connect.c \
	gvariant-builder.c \
	gvariant-get.c \
	gvariant-get-child.c \
	gvariant-iter.c \
	gvariant-lookup.c \
	gvariant-new.c \
	non-glib.c \
	nonnull.c \
	gerror-api.c \
	$(NULL)

templates = \
	assertion.head.c \
	assertion.tail.c \
	assertion-return.head.c \
	assertion-return.tail.c \
	generic.head.c \
	generic.tail.c \
	generic-non-glib.head.c \
	generic-non-glib.tail.c \
	gerror.head.c \
	gerror.tail.c \
	gsignal.head.c \
	gsignal.tail.c \
	gvariant.head.c \
	gvariant.tail.c \
	$(NULL)

TESTS = $(c_tests)
EXTRA_DIST = \
	$(templates) \
	$(c_tests) \
	wrapper-compiler-errors \
	$(NULL)

-include $(top_srcdir)/git.mk
