/* sample_main.c : demo of VFS snapshot and restore */
/* Copyright (c) 2026 Jon Mayo <jon@rm-f.net>
 * Licensed under BSD-2-Clause-Patent OR MIT */

#define _POSIX_C_SOURCE 200809L

#include "vfs-snap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char tmpdir[] = "/tmp/smolvfs_demo_XXXXXX";

static void
cleanup(void)
{
	char cmd[512];

	snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
	if (system(cmd)) { /* best effort */ }
}

static const struct vfs_cred admin = {
	.uid = 0,
	.is_admin = 1,
};

/****************************************************************
 * Tree printer
 ****************************************************************/

struct dir_list {
	char paths[64][4096];
	int count;
};

static int
print_and_collect(const struct vfs_stat *st, void *ctx)
{
	struct dir_list *dl = ctx;

	if (st->type == VFS_DIR) {
		printf("  [dir]  %s\n", st->path);
		if (dl->count < 64)
			snprintf(dl->paths[dl->count++], 4096,
			         "%s", st->path);
	} else {
		printf("  [file] %s  (%llu bytes)\n", st->path,
		       (unsigned long long)st->size);
	}
	return 0;
}

static void
print_tree(struct vfs *fs, const char *dirpath)
{
	struct dir_list dl = { .count = 0 };

	vfs_list(fs, &admin, dirpath, print_and_collect, &dl);

	for (int i = 0; i < dl.count; i++)
		print_tree(fs, dl.paths[i]);
}

/****************************************************************
 * Log printer
 ****************************************************************/

static int
print_log_entry(const char *hash, int64_t time_s, int32_t time_ns,
                const char *comment, void *ctx)
{
	(void)time_ns;
	(void)ctx;
	printf("  %.16s...  t=%lld  %s\n", hash,
	       (long long)time_s, comment);
	return 0;
}

/****************************************************************
 * First-entry log reader (grabs v1 hash)
 ****************************************************************/

struct first_hash_ctx {
	char hash[CAS_HASH_HEX + 1];
	int found;
};

static int
grab_first(const char *hash, int64_t time_s, int32_t time_ns,
           const char *comment, void *ctx)
{
	(void)time_s;
	(void)time_ns;
	(void)comment;
	struct first_hash_ctx *fc = ctx;

	if (!fc->found) {
		memcpy(fc->hash, hash, CAS_HASH_HEX + 1);
		fc->found = 1;
	}
	return 0;
}

/****************************************************************
 * Main
 ****************************************************************/

int
main(void)
{
	if (!mkdtemp(tmpdir)) {
		perror("mkdtemp");
		return 1;
	}
	atexit(cleanup);

	struct cas *store = cas_new(tmpdir);
	struct cas_tree *ct = cas_tree_new(store);
	struct vfs *fs = vfs_new(NULL);

	if (!store || !ct || !fs) {
		fprintf(stderr, "init failed\n");
		return 1;
	}

	/* --- v1 --- */
	printf("=== creating v1 ===\n");
	vfs_write(fs, &admin, "/README", "smolvfs v1\n", 11, 1);
	vfs_mkdir(fs, &admin, "/src", 0);
	vfs_write(fs, &admin, "/src/main.c",
	          "int main() { return 0; }\n", 25, 0);
	vfs_write(fs, &admin, "/src/util.c",
	          "void help() {}\n", 15, 0);

	vfs_snap_commit(fs, &admin, ct, "main", "initial version");
	print_tree(fs, "/");

	/* --- v2 --- */
	printf("\n=== creating v2 ===\n");
	vfs_write(fs, &admin, "/README",
	          "smolvfs v2 -- updated\n", 22, 0);
	vfs_mkdir(fs, &admin, "/doc", 0);
	vfs_write(fs, &admin, "/doc/api.txt",
	          "API reference goes here\n", 24, 0);
	vfs_delete(fs, &admin, "/src/util.c");

	vfs_snap_commit(fs, &admin, ct, "main", "add docs, drop util");
	print_tree(fs, "/");

	/* --- log --- */
	printf("\n=== commit log for 'main' ===\n");
	cas_tree_log_read(ct, "main", print_log_entry, NULL);

	/* --- restore v1 --- */
	printf("\n=== restoring v1 ===\n");
	struct first_hash_ctx fc = { .found = 0 };

	cas_tree_log_read(ct, "main", grab_first, &fc);

	struct vfs *v1 = vfs_new(NULL);

	vfs_snap_restore(v1, &admin, ct, fc.hash);
	print_tree(v1, "/");

	const void *data;
	size_t len;

	if (vfs_read(v1, &admin, "/README", &data, &len) == VFS_OK)
		printf("  /README: %.*s", (int)len, (const char *)data);

	/* --- restore v2 (checkout head of ref) --- */
	printf("\n=== checkout 'main' (v2) ===\n");
	struct vfs *v2 = vfs_new(NULL);

	vfs_snap_checkout(v2, &admin, ct, "main");
	print_tree(v2, "/");

	if (vfs_read(v2, &admin, "/README", &data, &len) == VFS_OK)
		printf("  /README: %.*s", (int)len, (const char *)data);

	vfs_free(v2);
	vfs_free(v1);
	vfs_free(fs);
	cas_tree_free(ct);
	cas_free(store);

	printf("\ndone.\n");
	return 0;
}
