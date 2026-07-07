# makefile for smolvfs
OBJCOPY ?= objcopy
STRIP ?= strip
RM ?= rm -f
CFLAGS := -Wall -Wextra -g -Og -fno-omit-frame-pointer
# CPPFLAGS := -NDEBUG
SRCS := sample_main.c
OBJS := $(SRCS:.c=.o)
DEPS := $(SRCS:.c=.dep)
LIB := libvfs.a
LIBSRCS := vfs.c cas.c cas-codec.c cas-tree.c cas-pack.c cas-omap.c vfs-snap.c
LIBOBJS := $(LIBSRCS:.c=.o)
DEPS += $(LIBSRCS:.c=.dep)

# Optional bundled DEFLATE codec (miniz).  Build with MINIZ=1 to
# vendor third_party/miniz.{c,h} and register the 'Z' codec.
ifdef MINIZ
CPPFLAGS += -DCAS_WITH_MINIZ -DMINIZ_NO_STDIO
MINIZ_OBJS := cas-codec-miniz.o third_party/miniz.o
endif

TEST_SRCS = test_cas.c test_cas_codec.c test_vfs.c test_cas_tree.c test_cas_pack.c test_cas_omap.c test_vfs_snap.c
TEST_BINS = $(TEST_SRCS:.c=)
TEST_OBJS = $(TEST_SRCS:.c=.o)
DEPS += $(TEST_SRCS:.c=.dep)
compile.c = $(CC) -c -o $@ -MMD -MF $(@:.o=.dep) $(CFLAGS) $(CPPFLAGS) $<
smolvfs: $(OBJS) $(LIB) $(MINIZ_OBJS)
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -lm
	$(OBJCOPY) --only-keep-debug $@ $@.debug
	$(STRIP) --strip-debug --strip-unneeded $@
	$(OBJCOPY) --add-gnu-debuglink=$@.debug $@
castool: castool.o cas-tree.o cas-pack.o cas.o cas-codec.o $(MINIZ_OBJS)
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS)
# Reference incremental HTTP downloader (examples/).  Needs libcurl-dev;
# build with MINIZ=1 for compressed depots.  Not built by default.
CURL_LIBS ?= -lcurl
cas-fetch: examples/cas-fetch.c cas.o cas-tree.o cas-pack.o cas-codec.o $(MINIZ_OBJS)
	$(CC) -o $@ $(CFLAGS) $(CPPFLAGS) -I. $(LDFLAGS) $^ $(CURL_LIBS) $(LDLIBS)
test_cas: test_cas.o cas-pack.o cas.o cas-codec.o $(MINIZ_OBJS)
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS)
test_vfs: test_vfs.o vfs.o
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -lm
test_cas_tree: test_cas_tree.o cas-tree.o cas-pack.o cas.o cas-codec.o $(MINIZ_OBJS)
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS)
test_vfs_snap: test_vfs_snap.o vfs-snap.o vfs.o cas-tree.o cas-pack.o cas.o cas-codec.o $(MINIZ_OBJS)
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -lm
$(LIB) : $(LIBOBJS)
	$(AR) $(ARFLAGS) $@ $^
vfs.o : vfs.c
	$(compile.c)
cas.o : cas.c
	$(compile.c)
cas-codec.o : cas-codec.c
	$(compile.c)
cas-codec-miniz.o : cas-codec-miniz.c
	$(compile.c)
third_party/miniz.o : third_party/miniz.c
	$(CC) -c -o $@ -MMD -MF $(@:.o=.dep) -O2 $(CPPFLAGS) $<
test_cas_codec: test_cas_codec.o cas-codec.o $(MINIZ_OBJS)
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS)
test_cas_codec.o : test_cas_codec.c
	$(compile.c)
sample_main.o : sample_main.c
	$(compile.c)
test_cas.o : test_cas.c
	$(compile.c)
cas-tree.o : cas-tree.c
	$(compile.c)
test_cas_tree.o : test_cas_tree.c
	$(compile.c)
test_vfs.o : test_vfs.c
	$(compile.c)
vfs-snap.o : vfs-snap.c
	$(compile.c)
test_vfs_snap.o : test_vfs_snap.c
	$(compile.c)
cas-pack.o : cas-pack.c
	$(compile.c)
test_cas_pack: test_cas_pack.o cas-pack.o cas.o cas-codec.o $(MINIZ_OBJS)
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS)
test_cas_pack.o : test_cas_pack.c
	$(compile.c)
test_cas_omap: test_cas_omap.o cas-omap.o cas-pack.o cas.o cas-codec.o $(MINIZ_OBJS)
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS)
test_cas_omap.o : test_cas_omap.c
	$(compile.c)
cas-omap.o : cas-omap.c
	$(compile.c)
castool.o : castool.c
	$(compile.c)
clean:
	$(RM) $(OBJS) $(TEST_OBJS) $(LIBOBJS) cas-codec-miniz.o third_party/miniz.o
clean-all: clean
	$(RM) smolvfs smolvfs.debug castool cas-fetch libvfs.a $(TEST_BINS) $(DEPS) third_party/miniz.dep
test: $(TEST_BINS)
	./test.sh
smoke: $(TEST_BINS)
	./test.sh
run: smolvfs
	./smolvfs
coverage: clean-all coverage-clean
	$(MAKE) CFLAGS="$(CFLAGS) --coverage" test
	lcov --capture --directory . --output-file coverage.info
	lcov --remove coverage.info '/usr/*' --output-file coverage.info
	genhtml coverage.info --output-directory coverage-html

coverage-clean:
	$(RM) -r coverage-html coverage.info *.gcda *.gcno

.PHONY: all clean clean-all test smoke run coverage coverage-clean
-include $(DEPS)
