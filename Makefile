# makefile for smolvfs
OBJCOPY ?= objcopy
STRIP ?= strip
RM ?= rm -f
CFLAGS := -Wall -Wextra -g -Og -fno-omit-frame-pointer
# CPPFLAGS := -NDEBUG
SRCS := sample_main.c
OBJS := $(SRCS:.c=.o)
DEPS := $(SRCS:.c=.d)
LIB := libvfs.a
LIBSRCS := vfs.c cas.c cas-tree.c cas-pack.c vfs-snap.c
LIBOBJS := $(LIBSRCS:.c=.o)
DEPS += $(LIBSRCS:.c=.d)
TEST_SRCS = test_cas.c test_vfs.c test_cas_tree.c test_cas_pack.c test_vfs_snap.c
TEST_BINS = $(TEST_SRCS:.c=)
TEST_OBJS = $(TEST_SRCS:.c=.o)
compile.c = $(CC) -c -o $@ -MMD -MF $(@:.o=.dep) $(CFLAGS) $(CPPFLAGS) $<
smolvfs: $(OBJS) $(LIB)
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -lm
	$(OBJCOPY) --only-keep-debug $@ $@.debug
	$(STRIP) --strip-debug --strip-unneeded $@
	$(OBJCOPY) --add-gnu-debuglink=$@.debug $@
castool: castool.o cas-tree.o cas-pack.o cas.o
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS)
test_cas: test_cas.o cas-pack.o cas.o
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS)
test_vfs: test_vfs.o vfs.o
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -lm
test_cas_tree: test_cas_tree.o cas-tree.o cas-pack.o cas.o
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS)
test_vfs_snap: test_vfs_snap.o vfs-snap.o vfs.o cas-tree.o cas-pack.o cas.o
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -lm
$(LIB) : $(LIBOBJS)
	$(AR) $(ARFLAGS) $@ $^
vfs.o : vfs.c
	$(compile.c)
cas.o : cas.c
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
test_cas_pack: test_cas_pack.o cas-pack.o cas.o
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS)
test_cas_pack.o : test_cas_pack.c
	$(compile.c)
castool.o : castool.c
	$(compile.c)
clean:
	$(RM) $(OBJS) $(TEST_OBJS)
clean-all: clean
	$(RM) smolvfs smolvfs.debug castool libvfs.a $(TEST_BINS) $(DEPS)
test: $(TEST_BINS)
	./test.sh
smoke: $(TEST_BINS)
	./test.sh
run: smolvfs
	./smolvfs
.PHONY: all clean clean-all test smoke run
-include $(DEPS)
