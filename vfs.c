/* vfs.c : in-memory virtual filesystem with Unix-style permissions and quotas */
/* Copyright (c) 2026 Jon Mayo <jon@rm-f.net>
 * Licensed under BSD-2-Clause-Patent OR MIT */

#define _POSIX_C_SOURCE 200809L

#include "vfs.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/****************************************************************
 * Internal constants
 ****************************************************************/

#define VFS_PATH_MAX    4096
#define VFS_INITIAL_CAP 64

/* Characters forbidden in a single filename component. */
static const char illegal_chars[] = ":/\\*?\"<>|";

/****************************************************************
 * Internal node representation
 ****************************************************************/

struct vfs_node {
	char           *path;       /* normalized absolute path */
	enum vfs_type   type;
	int             mode;
	int             owner;
	uint64_t        size;
	time_t          ctime;
	time_t          mtime;
	void           *data;       /* file contents (malloc'd) */
};

struct vfs {
	struct vfs_node *nodes;
	int              count;
	int              cap;
	uint64_t         usage;
	uint64_t         quota;
	int              umask;
};

/****************************************************************
 * Error strings
 ****************************************************************/

const char *
vfs_strerror(int err)
{
	switch (err) {
	case VFS_OK:        return "success";
	case VFS_ERR:       return "generic error";
	case VFS_ENOTFOUND: return "not found";
	case VFS_EEXIST:    return "already exists";
	case VFS_EPERM:     return "permission denied";
	case VFS_EQUOTA:    return "quota exceeded";
	case VFS_ENOTDIR:   return "not a directory";
	case VFS_EISDIR:    return "is a directory";
	case VFS_ENOTEMPTY: return "directory not empty";
	case VFS_EBADPATH:  return "invalid path";
	case VFS_ENOMEM:    return "out of memory";
	default:            return "unknown error";
	}
}

/****************************************************************
 * Path utilities
 ****************************************************************/

int
vfs_normalize(char *buf, size_t bufsz, const char *path)
{
	if (!path || !path[0] || path[0] != '/')
		return VFS_EBADPATH;

	/* Tokenize into components, resolve . and .. */
	char tmp[VFS_PATH_MAX];
	size_t tlen = strlen(path);
	if (tlen >= VFS_PATH_MAX)
		return VFS_EBADPATH;
	memcpy(tmp, path, tlen + 1);

	const char *parts[VFS_PATH_MAX / 2];
	int depth = 0;

	char *tok = tmp + 1; /* skip leading / */
	while (*tok) {
		/* skip consecutive slashes */
		while (*tok == '/')
			tok++;
		if (!*tok)
			break;
		char *end = strchr(tok, '/');
		if (end)
			*end = '\0';

		if (strcmp(tok, ".") == 0) {
			/* skip */
		} else if (strcmp(tok, "..") == 0) {
			if (depth > 0)
				depth--;
		} else {
			parts[depth++] = tok;
		}

		if (!end)
			break;
		tok = end + 1;
	}

	/* Rebuild path */
	if (depth == 0) {
		if (bufsz < 2)
			return VFS_EBADPATH;
		buf[0] = '/';
		buf[1] = '\0';
		return VFS_OK;
	}

	size_t pos = 0;
	for (int i = 0; i < depth; i++) {
		size_t plen = strlen(parts[i]);
		if (pos + 1 + plen + 1 > bufsz)
			return VFS_EBADPATH;
		buf[pos++] = '/';
		memcpy(buf + pos, parts[i], plen);
		pos += plen;
	}
	buf[pos] = '\0';
	return VFS_OK;
}

int
vfs_resolve(char *buf, size_t bufsz,
            const char *path, const char *cwd)
{
	if (!path || !path[0])
		return VFS_EBADPATH;

	if (path[0] == '/')
		return vfs_normalize(buf, bufsz, path);

	/* Relative: prepend cwd */
	char tmp[VFS_PATH_MAX];
	size_t cwdlen = cwd ? strlen(cwd) : 1;
	size_t plen = strlen(path);
	if (cwdlen + 1 + plen + 1 > VFS_PATH_MAX)
		return VFS_EBADPATH;

	if (cwd && cwd[0] == '/') {
		memcpy(tmp, cwd, cwdlen);
	} else {
		tmp[0] = '/';
		cwdlen = 1;
	}
	tmp[cwdlen] = '/';
	memcpy(tmp + cwdlen + 1, path, plen + 1);

	return vfs_normalize(buf, bufsz, tmp);
}

/** Extract the parent directory of a normalized path.
 *  Returns "/" for top-level paths.
 */
static int
path_parent(char *buf, size_t bufsz, const char *path)
{
	if (strcmp(path, "/") == 0)
		return VFS_ENOTFOUND;

	const char *last = strrchr(path, '/');
	if (last == path) {
		if (bufsz < 2)
			return VFS_EBADPATH;
		buf[0] = '/';
		buf[1] = '\0';
		return VFS_OK;
	}

	size_t len = (size_t)(last - path);
	if (len + 1 > bufsz)
		return VFS_EBADPATH;
	memcpy(buf, path, len);
	buf[len] = '\0';
	return VFS_OK;
}

/** Validate a single filename component (no path separators). */
static int
validate_filename(const char *name)
{
	if (!name || !name[0] || strcmp(name, ".") == 0 ||
	    strcmp(name, "..") == 0)
		return VFS_EBADPATH;

	for (const char *p = name; *p; p++) {
		if ((unsigned char)*p < 0x20)
			return VFS_EBADPATH;
		if (strchr(illegal_chars, *p))
			return VFS_EBADPATH;
	}
	return VFS_OK;
}

/** Validate all components of a normalized path. */
static int
validate_path_components(const char *path)
{
	if (strcmp(path, "/") == 0)
		return VFS_OK;

	char tmp[VFS_PATH_MAX];
	size_t len = strlen(path);
	if (len >= VFS_PATH_MAX)
		return VFS_EBADPATH;
	memcpy(tmp, path, len + 1);

	char *tok = tmp + 1;
	while (*tok) {
		char *slash = strchr(tok, '/');
		if (slash)
			*slash = '\0';
		int rc = validate_filename(tok);
		if (rc != VFS_OK)
			return rc;
		if (!slash)
			break;
		tok = slash + 1;
	}
	return VFS_OK;
}

/****************************************************************
 * Node storage
 ****************************************************************/

static struct vfs_node *
find_node(struct vfs *fs, const char *path)
{
	for (int i = 0; i < fs->count; i++) {
		if (strcmp(fs->nodes[i].path, path) == 0)
			return &fs->nodes[i];
	}
	return NULL;
}

static int
grow_nodes(struct vfs *fs)
{
	int newcap = fs->cap * 2;
	struct vfs_node *tmp = realloc(fs->nodes,
	                               (size_t)newcap * sizeof(*tmp));
	if (!tmp)
		return VFS_ENOMEM;
	fs->nodes = tmp;
	fs->cap = newcap;
	return VFS_OK;
}

static struct vfs_node *
add_node(struct vfs *fs, const char *path, enum vfs_type type,
         int mode, int owner)
{
	if (fs->count >= fs->cap) {
		if (grow_nodes(fs) != VFS_OK)
			return NULL;
	}

	struct vfs_node *n = &fs->nodes[fs->count];
	n->path = strdup(path);
	if (!n->path)
		return NULL;
	n->type = type;
	n->mode = mode & 0777;
	n->owner = owner;
	n->size = 0;
	n->ctime = time(NULL);
	n->mtime = n->ctime;
	n->data = NULL;
	fs->count++;
	return n;
}

static void
free_node_contents(struct vfs_node *n)
{
	free(n->path);
	free(n->data);
	n->path = NULL;
	n->data = NULL;
}

static void
remove_node(struct vfs *fs, struct vfs_node *n)
{
	if (n->type == VFS_FILE)
		fs->usage -= n->size;
	free_node_contents(n);

	int idx = (int)(n - fs->nodes);
	if (idx < fs->count - 1)
		fs->nodes[idx] = fs->nodes[fs->count - 1];
	fs->count--;
}

static void
fill_stat(const struct vfs_node *n, struct vfs_stat *st)
{
	st->path = n->path;
	st->type = n->type;
	st->mode = n->mode;
	st->owner = n->owner;
	st->size = n->size;
	st->ctime = n->ctime;
	st->mtime = n->mtime;
}

/****************************************************************
 * Permission checks
 ****************************************************************/

static int
check_perm(const struct vfs_node *n, const struct vfs_cred *cred,
           int perm)
{
	if (cred->is_admin)
		return 1;

	/* owner bits */
	if (cred->uid == n->owner) {
		if (n->mode & (perm << 6))
			return 1;
	}

	/* group bits */
	if (cred->is_group) {
		if (n->mode & (perm << 3))
			return 1;
	}

	/* other bits */
	if (n->mode & perm)
		return 1;

	return 0;
}

/* Permission bit constants for check_perm's perm argument */
#define PERM_R 04
#define PERM_W 02

/****************************************************************
 * Lifecycle
 ****************************************************************/

struct vfs *
vfs_new(const struct vfs_opts *opts)
{
	struct vfs *fs = calloc(1, sizeof(*fs));
	if (!fs)
		return NULL;

	fs->cap = VFS_INITIAL_CAP;
	fs->nodes = calloc((size_t)fs->cap, sizeof(*fs->nodes));
	if (!fs->nodes) {
		free(fs);
		return NULL;
	}

	if (opts) {
		fs->quota = opts->quota;
		fs->umask = opts->umask & 0777;
	}
	return fs;
}

void
vfs_free(struct vfs *fs)
{
	if (!fs)
		return;
	for (int i = 0; i < fs->count; i++)
		free_node_contents(&fs->nodes[i]);
	free(fs->nodes);
	free(fs);
}

/****************************************************************
 * Stat
 ****************************************************************/

int
vfs_stat(struct vfs *fs, const struct vfs_cred *cred,
         const char *path, struct vfs_stat *st)
{
	char norm[VFS_PATH_MAX];
	int rc = vfs_normalize(norm, sizeof(norm), path);
	if (rc != VFS_OK)
		return rc;

	/* Root directory is implicit */
	if (strcmp(norm, "/") == 0) {
		st->path = "/";
		st->type = VFS_DIR;
		st->mode = VFS_MODE_DIR_DEFAULT;
		st->owner = 0;
		st->size = 0;
		st->ctime = 0;
		st->mtime = 0;
		return VFS_OK;
	}

	struct vfs_node *n = find_node(fs, norm);
	if (!n)
		return VFS_ENOTFOUND;

	if (!check_perm(n, cred, PERM_R))
		return VFS_EPERM;

	fill_stat(n, st);
	return VFS_OK;
}

/****************************************************************
 * List directory
 ****************************************************************/

int
vfs_list(struct vfs *fs, const struct vfs_cred *cred,
         const char *dirpath,
         int (*fn)(const struct vfs_stat *st, void *ctx),
         void *ctx)
{
	char norm[VFS_PATH_MAX];
	int rc = vfs_normalize(norm, sizeof(norm), dirpath);
	if (rc != VFS_OK)
		return rc;

	/* Verify directory exists (root is always valid) */
	if (strcmp(norm, "/") != 0) {
		struct vfs_node *dir = find_node(fs, norm);
		if (!dir)
			return VFS_ENOTFOUND;
		if (dir->type != VFS_DIR)
			return VFS_ENOTDIR;
		if (!check_perm(dir, cred, PERM_R))
			return VFS_EPERM;
	}

	size_t dirlen = strlen(norm);
	int is_root = (strcmp(norm, "/") == 0);

	for (int i = 0; i < fs->count; i++) {
		struct vfs_node *n = &fs->nodes[i];
		const char *p = n->path;

		/* Must be under this directory */
		if (is_root) {
			/* Direct child of root: /foo but not /foo/bar */
			if (p[0] != '/' || strlen(p) < 2)
				continue;
			if (strchr(p + 1, '/'))
				continue;
		} else {
			/* Must start with dir + "/" */
			if (strncmp(p, norm, dirlen) != 0)
				continue;
			if (p[dirlen] != '/')
				continue;
			/* Must be direct child (no further slashes) */
			if (strchr(p + dirlen + 1, '/'))
				continue;
		}

		struct vfs_stat st;
		fill_stat(n, &st);
		rc = fn(&st, ctx);
		if (rc != 0)
			return rc;
	}
	return VFS_OK;
}

/****************************************************************
 * Read
 ****************************************************************/

int
vfs_read(struct vfs *fs, const struct vfs_cred *cred,
         const char *path, const void **out, size_t *out_len)
{
	char norm[VFS_PATH_MAX];
	int rc = vfs_normalize(norm, sizeof(norm), path);
	if (rc != VFS_OK)
		return rc;

	struct vfs_node *n = find_node(fs, norm);
	if (!n)
		return VFS_ENOTFOUND;
	if (n->type == VFS_DIR)
		return VFS_EISDIR;
	if (!check_perm(n, cred, PERM_R))
		return VFS_EPERM;

	*out = n->data;
	*out_len = (size_t)n->size;
	return VFS_OK;
}

/****************************************************************
 * Internal: ensure parent directory chain exists
 ****************************************************************/

static int
ensure_parents(struct vfs *fs, const struct vfs_cred *cred,
               const char *path)
{
	char parent[VFS_PATH_MAX];
	int rc = path_parent(parent, sizeof(parent), path);
	if (rc != VFS_OK)
		return VFS_OK; /* at root, nothing to create */

	if (strcmp(parent, "/") == 0)
		return VFS_OK; /* root always exists */

	struct vfs_node *pn = find_node(fs, parent);
	if (pn) {
		if (pn->type != VFS_DIR)
			return VFS_ENOTDIR;
		return VFS_OK;
	}

	/* Recursively create grandparent first */
	rc = ensure_parents(fs, cred, parent);
	if (rc != VFS_OK)
		return rc;

	int mode = VFS_MODE_DIR_DEFAULT & ~fs->umask;
	if (!add_node(fs, parent, VFS_DIR, mode, cred->uid))
		return VFS_ENOMEM;
	return VFS_OK;
}

/****************************************************************
 * Write
 ****************************************************************/

int
vfs_write(struct vfs *fs, const struct vfs_cred *cred,
          const char *path, const void *data, size_t len,
          int create_parents)
{
	char norm[VFS_PATH_MAX];
	int rc = vfs_normalize(norm, sizeof(norm), path);
	if (rc != VFS_OK)
		return rc;

	if (strcmp(norm, "/") == 0)
		return VFS_EISDIR;

	rc = validate_path_components(norm);
	if (rc != VFS_OK)
		return rc;

	/* Check quota */
	struct vfs_node *existing = find_node(fs, norm);
	uint64_t old_size = 0;
	if (existing) {
		if (existing->type == VFS_DIR)
			return VFS_EISDIR;
		if (!check_perm(existing, cred, PERM_W))
			return VFS_EPERM;
		old_size = existing->size;
	}

	if (fs->quota > 0) {
		uint64_t effective = fs->usage - old_size;
		if (effective + len > fs->quota)
			return VFS_EQUOTA;
	}

	/* Ensure parent exists */
	if (create_parents) {
		rc = ensure_parents(fs, cred, norm);
		if (rc != VFS_OK)
			return rc;
	} else {
		char parent[VFS_PATH_MAX];
		rc = path_parent(parent, sizeof(parent), norm);
		if (rc == VFS_OK && strcmp(parent, "/") != 0) {
			struct vfs_node *pn = find_node(fs, parent);
			if (!pn)
				return VFS_ENOTFOUND;
			if (pn->type != VFS_DIR)
				return VFS_ENOTDIR;
		}
	}

	/* Allocate content */
	void *copy = NULL;
	if (len > 0) {
		copy = malloc(len);
		if (!copy)
			return VFS_ENOMEM;
		memcpy(copy, data, len);
	}

	if (existing) {
		/* Overwrite */
		free(existing->data);
		existing->data = copy;
		fs->usage -= old_size;
		existing->size = len;
		fs->usage += len;
		existing->mtime = time(NULL);
		return VFS_OK;
	}

	/* Create new node */
	int mode = VFS_MODE_FILE_DEFAULT & ~fs->umask;
	struct vfs_node *n = add_node(fs, norm, VFS_FILE, mode,
	                              cred->uid);
	if (!n) {
		free(copy);
		return VFS_ENOMEM;
	}
	n->data = copy;
	n->size = len;
	fs->usage += len;
	return VFS_OK;
}

/****************************************************************
 * Mkdir
 ****************************************************************/

int
vfs_mkdir(struct vfs *fs, const struct vfs_cred *cred,
          const char *path, int create_parents)
{
	char norm[VFS_PATH_MAX];
	int rc = vfs_normalize(norm, sizeof(norm), path);
	if (rc != VFS_OK)
		return rc;

	if (strcmp(norm, "/") == 0)
		return VFS_OK; /* root always exists */

	rc = validate_path_components(norm);
	if (rc != VFS_OK)
		return rc;

	struct vfs_node *existing = find_node(fs, norm);
	if (existing) {
		if (existing->type == VFS_DIR && create_parents)
			return VFS_OK; /* mkdir -p: no error */
		if (existing->type == VFS_DIR)
			return VFS_EEXIST;
		return VFS_EEXIST;
	}

	if (create_parents) {
		rc = ensure_parents(fs, cred, norm);
		if (rc != VFS_OK)
			return rc;
	} else {
		char parent[VFS_PATH_MAX];
		rc = path_parent(parent, sizeof(parent), norm);
		if (rc == VFS_OK && strcmp(parent, "/") != 0) {
			struct vfs_node *pn = find_node(fs, parent);
			if (!pn)
				return VFS_ENOTFOUND;
			if (pn->type != VFS_DIR)
				return VFS_ENOTDIR;
		}
	}

	int mode = VFS_MODE_DIR_DEFAULT & ~fs->umask;
	if (!add_node(fs, norm, VFS_DIR, mode, cred->uid))
		return VFS_ENOMEM;
	return VFS_OK;
}

/****************************************************************
 * Delete
 ****************************************************************/

/** Count direct children of a path. */
static int
count_children(struct vfs *fs, const char *dirpath)
{
	size_t dirlen = strlen(dirpath);
	int is_root = (strcmp(dirpath, "/") == 0);
	int count = 0;

	for (int i = 0; i < fs->count; i++) {
		const char *p = fs->nodes[i].path;
		if (is_root) {
			if (p[0] == '/' && strlen(p) > 1 &&
			    !strchr(p + 1, '/'))
				count++;
		} else {
			if (strncmp(p, dirpath, dirlen) == 0 &&
			    p[dirlen] == '/')
				count++;
		}
	}
	return count;
}

int
vfs_delete(struct vfs *fs, const struct vfs_cred *cred,
           const char *path)
{
	char norm[VFS_PATH_MAX];
	int rc = vfs_normalize(norm, sizeof(norm), path);
	if (rc != VFS_OK)
		return rc;

	if (strcmp(norm, "/") == 0)
		return VFS_EPERM; /* cannot delete root */

	struct vfs_node *n = find_node(fs, norm);
	if (!n)
		return VFS_ENOTFOUND;

	if (!check_perm(n, cred, PERM_W))
		return VFS_EPERM;

	if (n->type == VFS_DIR && count_children(fs, norm) > 0)
		return VFS_ENOTEMPTY;

	remove_node(fs, n);
	return VFS_OK;
}

int
vfs_delete_recursive(struct vfs *fs, const struct vfs_cred *cred,
                     const char *path)
{
	char norm[VFS_PATH_MAX];
	int rc = vfs_normalize(norm, sizeof(norm), path);
	if (rc != VFS_OK)
		return rc;

	if (strcmp(norm, "/") == 0)
		return VFS_EPERM;

	struct vfs_node *n = find_node(fs, norm);
	if (!n)
		return VFS_ENOTFOUND;

	if (!check_perm(n, cred, PERM_W))
		return VFS_EPERM;

	if (n->type != VFS_DIR) {
		remove_node(fs, n);
		return VFS_OK;
	}

	/* Delete all descendants (iterate backwards to handle
	 * swap-removal safely) */
	size_t pfxlen = strlen(norm);
	for (int i = fs->count - 1; i >= 0; i--) {
		const char *p = fs->nodes[i].path;
		if (strncmp(p, norm, pfxlen) == 0 && p[pfxlen] == '/')
			remove_node(fs, &fs->nodes[i]);
	}

	/* Re-find the directory node (index may have changed) */
	n = find_node(fs, norm);
	if (n)
		remove_node(fs, n);
	return VFS_OK;
}

/****************************************************************
 * Rename
 ****************************************************************/

int
vfs_rename(struct vfs *fs, const struct vfs_cred *cred,
           const char *old_path, const char *new_path)
{
	char onorm[VFS_PATH_MAX], nnorm[VFS_PATH_MAX];
	int rc;

	rc = vfs_normalize(onorm, sizeof(onorm), old_path);
	if (rc != VFS_OK)
		return rc;
	rc = vfs_normalize(nnorm, sizeof(nnorm), new_path);
	if (rc != VFS_OK)
		return rc;

	if (strcmp(onorm, "/") == 0 || strcmp(nnorm, "/") == 0)
		return VFS_EPERM;

	rc = validate_path_components(nnorm);
	if (rc != VFS_OK)
		return rc;

	if (strcmp(onorm, nnorm) == 0)
		return VFS_OK; /* same path, no-op */

	/* Cannot move into own subtree */
	size_t olen = strlen(onorm);
	if (strncmp(nnorm, onorm, olen) == 0 && nnorm[olen] == '/')
		return VFS_EBADPATH;

	struct vfs_node *src = find_node(fs, onorm);
	if (!src)
		return VFS_ENOTFOUND;

	if (!check_perm(src, cred, PERM_W))
		return VFS_EPERM;

	/* Check destination */
	struct vfs_node *dst = find_node(fs, nnorm);
	if (dst) {
		if (dst->type == VFS_DIR && count_children(fs, nnorm) > 0)
			return VFS_ENOTEMPTY;
		remove_node(fs, dst);
		/* src pointer may be invalidated by remove_node */
		src = find_node(fs, onorm);
		if (!src)
			return VFS_ERR;
	}

	/* Ensure destination parent exists */
	char parent[VFS_PATH_MAX];
	rc = path_parent(parent, sizeof(parent), nnorm);
	if (rc == VFS_OK && strcmp(parent, "/") != 0) {
		struct vfs_node *pn = find_node(fs, parent);
		if (!pn)
			return VFS_ENOTFOUND;
		if (pn->type != VFS_DIR)
			return VFS_ENOTDIR;
	}

	/* Rename descendants (if directory) */
	if (src->type == VFS_DIR) {
		size_t old_pfx = strlen(onorm);
		for (int i = 0; i < fs->count; i++) {
			struct vfs_node *c = &fs->nodes[i];
			if (strncmp(c->path, onorm, old_pfx) != 0 ||
			    c->path[old_pfx] != '/')
				continue;

			const char *suffix = c->path + old_pfx;
			size_t slen = strlen(suffix);
			size_t nlen = strlen(nnorm);
			char *newp = malloc(nlen + slen + 1);
			if (!newp)
				return VFS_ENOMEM;
			memcpy(newp, nnorm, nlen);
			memcpy(newp + nlen, suffix, slen + 1);
			free(c->path);
			c->path = newp;
			c->mtime = time(NULL);
		}
	}

	/* Rename the node itself */
	char *newp = strdup(nnorm);
	if (!newp)
		return VFS_ENOMEM;
	free(src->path);
	src->path = newp;
	src->mtime = time(NULL);
	return VFS_OK;
}

/****************************************************************
 * Copy
 ****************************************************************/

int
vfs_copy(struct vfs *fs, const struct vfs_cred *cred,
         const char *src_path, const char *dst_path,
         int create_parents)
{
	char snorm[VFS_PATH_MAX];
	int rc = vfs_normalize(snorm, sizeof(snorm), src_path);
	if (rc != VFS_OK)
		return rc;

	struct vfs_node *src = find_node(fs, snorm);
	if (!src)
		return VFS_ENOTFOUND;
	if (src->type == VFS_DIR)
		return VFS_EISDIR;
	if (!check_perm(src, cred, PERM_R))
		return VFS_EPERM;

	/* Copy data before vfs_write (which may reallocate nodes) */
	void *datacopy = NULL;
	size_t datalen = (size_t)src->size;
	if (datalen > 0) {
		datacopy = malloc(datalen);
		if (!datacopy)
			return VFS_ENOMEM;
		memcpy(datacopy, src->data, datalen);
	}

	rc = vfs_write(fs, cred, dst_path, datacopy, datalen,
	               create_parents);
	free(datacopy);
	return rc;
}

/****************************************************************
 * Chmod
 ****************************************************************/

int
vfs_chmod(struct vfs *fs, const struct vfs_cred *cred,
          const char *path, int mode)
{
	if (!cred->is_admin)
		return VFS_EPERM;

	char norm[VFS_PATH_MAX];
	int rc = vfs_normalize(norm, sizeof(norm), path);
	if (rc != VFS_OK)
		return rc;

	struct vfs_node *n = find_node(fs, norm);
	if (!n)
		return VFS_ENOTFOUND;

	n->mode = mode & 0777;
	n->mtime = time(NULL);
	return VFS_OK;
}

/****************************************************************
 * Quota
 ****************************************************************/

uint64_t
vfs_usage(struct vfs *fs)
{
	return fs->usage;
}

uint64_t
vfs_quota(struct vfs *fs)
{
	return fs->quota;
}

/****************************************************************
 * fnmatch (case-insensitive glob matching)
 ****************************************************************/

int
vfs_fnmatch(const char *pat, const char *name)
{
	while (*pat) {
		if (*pat == '*') {
			pat++;
			/* Skip consecutive stars */
			while (*pat == '*')
				pat++;
			if (!*pat)
				return 1; /* trailing * matches all */
			for (const char *t = name; *t; t++) {
				if (vfs_fnmatch(pat, t))
					return 1;
			}
			return 0;
		}

		if (*pat == '?') {
			if (!*name || *name == '/')
				return 0;
			pat++;
			name++;
			continue;
		}

		if (*pat == '[') {
			pat++;
			int negate = 0;
			if (*pat == '!' || *pat == '^') {
				negate = 1;
				pat++;
			}
			int matched = 0;
			char c = (char)tolower((unsigned char)*name);
			while (*pat && *pat != ']') {
				char lo = (char)tolower((unsigned char)*pat);
				pat++;
				if (*pat == '-' && pat[1] && pat[1] != ']') {
					pat++;
					char hi = (char)tolower(
					    (unsigned char)*pat);
					pat++;
					if (c >= lo && c <= hi)
						matched = 1;
				} else {
					if (c == lo)
						matched = 1;
				}
			}
			if (*pat == ']')
				pat++;
			if (negate)
				matched = !matched;
			if (!matched)
				return 0;
			name++;
			continue;
		}

		/* Literal character */
		if (tolower((unsigned char)*pat) !=
		    tolower((unsigned char)*name))
			return 0;
		pat++;
		name++;
	}

	return *name == '\0';
}
