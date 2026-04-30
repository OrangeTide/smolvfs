/* cas-pack.c : packfile format for content-addressable store */
/* Copyright (c) 2026 Jon Mayo <jon@rm-f.net>
 * Licensed under BSD-2-Clause-Patent OR MIT */

#define _POSIX_C_SOURCE 200809L

#include "cas-pack.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/****************************************************************
 * Data structures
 ****************************************************************/

struct cas_pack {
	void *map;
	size_t maplen;
	uint64_t entry_count;
	const struct cas_pack_index_entry *index;
};

/****************************************************************
 * Helpers
 ****************************************************************/

static int
write_full(int fd, const void *data, size_t len)
{
	const unsigned char *p = data;

	while (len > 0) {
		ssize_t n = write(fd, p, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return CAS_EIO;
		}
		p += n;
		len -= (size_t)n;
	}
	return CAS_OK;
}

static int
format_header(char *buf, size_t bufsz, const char *type, size_t len)
{
	int n = snprintf(buf, bufsz, "%s %zu", type, len);

	if (n < 0 || (size_t)n + 1 > bufsz)
		return -1;
	return n + 1;
}

static int
parse_header(const char *hdr, size_t hdrsz,
             char *type_out, size_t type_bufsz,
             size_t *content_len)
{
	size_t limit = hdrsz < 32 ? hdrsz : 32;
	const char *nul = memchr(hdr, '\0', limit);

	if (!nul)
		return CAS_ERR;

	const char *sp = memchr(hdr, ' ', (size_t)(nul - hdr));

	if (!sp)
		return CAS_ERR;

	size_t typelen = (size_t)(sp - hdr);

	if (typelen == 0 || typelen > CAS_TYPE_MAX ||
	    typelen + 1 > type_bufsz)
		return CAS_ERR;

	const char *d = sp + 1;

	if (d >= nul)
		return CAS_ERR;

	size_t len = 0;

	for (; d < nul; d++) {
		if (*d < '0' || *d > '9')
			return CAS_ERR;
		size_t prev = len;

		len = len * 10 + (size_t)(*d - '0');
		if (len < prev)
			return CAS_ERR;
	}

	memcpy(type_out, hdr, typelen);
	type_out[typelen] = '\0';
	*content_len = len;
	return CAS_OK;
}

/****************************************************************
 * Lifecycle
 ****************************************************************/

struct cas_pack *
cas_pack_open(const char *path)
{
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return NULL;

	struct stat sb;

	if (fstat(fd, &sb) != 0) {
		close(fd);
		return NULL;
	}

	size_t filesz = (size_t)sb.st_size;

	if (filesz < CAS_PACK_BLOCK) {
		close(fd);
		return NULL;
	}

	void *map = mmap(NULL, filesz, PROT_READ, MAP_PRIVATE, fd, 0);

	close(fd);
	if (map == MAP_FAILED)
		return NULL;

	const struct cas_pack_footer *ft =
		(const struct cas_pack_footer *)
		((char *)map + filesz - CAS_PACK_BLOCK);

	static const unsigned char footer_magic[] =
		CAS_PACK_FOOTER_MAGIC_V1;

	if (memcmp(ft->magic, footer_magic, CAS_PACK_MAGIC_LEN) != 0) {
		munmap(map, filesz);
		return NULL;
	}

	uint64_t count = ft->entry_count;
	size_t index_size = (size_t)count * CAS_PACK_BLOCK;
	size_t tail_size = index_size + CAS_PACK_BLOCK;

	if (tail_size > filesz) {
		munmap(map, filesz);
		return NULL;
	}

	const struct cas_pack_index_entry *index =
		(const struct cas_pack_index_entry *)
		((char *)map + filesz - tail_size);

	size_t check_len = index_size + 16;
	unsigned char expected[CAS_HASH_LEN];

	cas_digest(index, check_len, expected);

	if (memcmp(expected, ft->checksum, CAS_HASH_LEN) != 0) {
		munmap(map, filesz);
		return NULL;
	}

	struct cas_pack *pack = calloc(1, sizeof(*pack));

	if (!pack) {
		munmap(map, filesz);
		return NULL;
	}

	pack->map = map;
	pack->maplen = filesz;
	pack->entry_count = count;
	pack->index = index;
	return pack;
}

void
cas_pack_close(struct cas_pack *pack)
{
	if (!pack)
		return;
	if (pack->map)
		munmap(pack->map, pack->maplen);
	free(pack);
}

/****************************************************************
 * Lookup
 ****************************************************************/

static const struct cas_pack_index_entry *
find_entry(const struct cas_pack *pack,
           const unsigned char *bin_hash)
{
	uint64_t lo = 0, hi = pack->entry_count;

	while (lo < hi) {
		uint64_t mid = lo + (hi - lo) / 2;
		int cmp = memcmp(pack->index[mid].hash, bin_hash,
		                 CAS_HASH_LEN);

		if (cmp < 0)
			lo = mid + 1;
		else if (cmp > 0)
			hi = mid;
		else
			return &pack->index[mid];
	}
	return NULL;
}

int
cas_pack_lookup(struct cas_pack *pack, struct cas_file *cf,
                const char *hash, char *type_out,
                size_t type_bufsz)
{
	unsigned char bin[CAS_HASH_LEN];

	if (cas_hex_decode(hash, CAS_HASH_HEX, bin,
	                   sizeof(bin)) != 0)
		return CAS_ERR;

	const struct cas_pack_index_entry *e =
		find_entry(pack, bin);

	if (!e)
		return CAS_ENOTFOUND;

	uint64_t trailer_off = e->offset;

	if (trailer_off + CAS_PACK_BLOCK > pack->maplen)
		return CAS_ERR;

	const struct cas_pack_trailer *tr =
		(const struct cas_pack_trailer *)
		((char *)pack->map + trailer_off);

	static const unsigned char trailer_magic[] =
		CAS_PACK_TRAILER_MAGIC;

	if (memcmp(tr->magic, trailer_magic,
	           CAS_PACK_MAGIC_LEN) != 0)
		return CAS_ERR;

	char type[CAS_TYPE_MAX + 1];
	size_t content_len;

	if (parse_header(tr->header, CAS_PACK_HEADER_LEN,
	                 type, sizeof(type),
	                 &content_len) != CAS_OK)
		return CAS_ERR;

	if (content_len > trailer_off)
		return CAS_ERR;

	if (type_out) {
		if (strlen(type) >= type_bufsz)
			return CAS_ERR;
		memcpy(type_out, type, strlen(type) + 1);
	}

	cf->_map = NULL;
	cf->_maplen = 0;

	if (content_len == 0) {
		cf->data = NULL;
		cf->len = 0;
	} else {
		cf->data = (const unsigned char *)pack->map +
		           trailer_off - content_len;
		cf->len = content_len;
	}
	return CAS_OK;
}

int
cas_pack_exists(struct cas_pack *pack, const char *hash)
{
	unsigned char bin[CAS_HASH_LEN];

	if (cas_hex_decode(hash, CAS_HASH_HEX, bin,
	                   sizeof(bin)) != 0)
		return 0;
	return find_entry(pack, bin) != NULL;
}

uint64_t
cas_pack_count(struct cas_pack *pack)
{
	return pack->entry_count;
}

/****************************************************************
 * Iteration
 ****************************************************************/

int
cas_pack_foreach(struct cas_pack *pack, cas_pack_foreach_fn fn,
                 void *ctx)
{
	for (uint64_t i = 0; i < pack->entry_count; i++) {
		char hex[CAS_HASH_HEX + 1];

		cas_hex_encode(pack->index[i].hash, CAS_HASH_LEN,
		               hex);
		if (fn(hex, ctx) != 0)
			break;
	}
	return CAS_OK;
}

/****************************************************************
 * Fsck
 ****************************************************************/

int
cas_pack_fsck(struct cas_pack *pack, cas_fsck_fn fn, void *ctx)
{
	int errors = 0;

	for (uint64_t i = 0; i < pack->entry_count; i++) {
		const struct cas_pack_index_entry *e =
			&pack->index[i];
		char hex[CAS_HASH_HEX + 1];
		int status;

		cas_hex_encode(e->hash, CAS_HASH_LEN, hex);

		uint64_t trailer_off = e->offset;

		if (trailer_off + CAS_PACK_BLOCK > pack->maplen) {
			status = CAS_FSCK_IOERR;
			goto report;
		}

		const struct cas_pack_trailer *tr =
			(const struct cas_pack_trailer *)
			((char *)pack->map + trailer_off);

		static const unsigned char trailer_magic[] =
			CAS_PACK_TRAILER_MAGIC;

		if (memcmp(tr->magic, trailer_magic,
		           CAS_PACK_MAGIC_LEN) != 0) {
			status = CAS_FSCK_CORRUPT;
			goto report;
		}

		char type[CAS_TYPE_MAX + 1];
		size_t content_len;

		if (parse_header(tr->header, CAS_PACK_HEADER_LEN,
		                 type, sizeof(type),
		                 &content_len) != CAS_OK) {
			status = CAS_FSCK_CORRUPT;
			goto report;
		}

		if (content_len > trailer_off) {
			status = CAS_FSCK_CORRUPT;
			goto report;
		}

		const unsigned char *data =
			(const unsigned char *)pack->map +
			trailer_off - content_len;

		unsigned char digest[CAS_HASH_LEN];

		cas_digest_object(type, data, content_len, digest);

		if (memcmp(digest, e->hash, CAS_HASH_LEN) != 0) {
			status = CAS_FSCK_CORRUPT;
			goto report;
		}

		status = CAS_FSCK_OK;

	report:
		if (status != CAS_FSCK_OK)
			errors++;
		if (fn && fn(hex, status, ctx) != 0)
			break;
	}

	return errors ? CAS_ERR : CAS_OK;
}

/****************************************************************
 * Create
 ****************************************************************/

struct hash_list {
	unsigned char (*hashes)[CAS_HASH_LEN];
	int count;
	int cap;
};

static int
collect_hash(const char *hash, void *ctx)
{
	struct hash_list *hl = ctx;

	if (hl->count >= hl->cap) {
		int newcap = hl->cap ? hl->cap * 2 : 64;
		void *p = realloc(hl->hashes,
		                   (size_t)newcap * CAS_HASH_LEN);

		if (!p)
			return -1;
		hl->hashes = p;
		hl->cap = newcap;
	}

	cas_hex_decode(hash, CAS_HASH_HEX,
	               hl->hashes[hl->count], CAS_HASH_LEN);
	hl->count++;
	return 0;
}

static int
hash_cmp(const void *a, const void *b)
{
	return memcmp(a, b, CAS_HASH_LEN);
}

int
cas_pack_create(struct cas *store, const char *path)
{
	struct hash_list hl = {0};

	cas_foreach(store, collect_hash, &hl);

	if (hl.count == 0) {
		free(hl.hashes);
		return CAS_OK;
	}

	qsort(hl.hashes, (size_t)hl.count, CAS_HASH_LEN, hash_cmp);

	int unique = 1;

	for (int i = 1; i < hl.count; i++) {
		if (memcmp(hl.hashes[i], hl.hashes[unique - 1],
		           CAS_HASH_LEN) != 0) {
			if (i != unique)
				memcpy(hl.hashes[unique],
				       hl.hashes[i], CAS_HASH_LEN);
			unique++;
		}
	}
	hl.count = unique;

	char tmp[512];

	if (snprintf(tmp, sizeof(tmp), "%s.XXXXXX", path) >=
	    (int)sizeof(tmp)) {
		free(hl.hashes);
		return CAS_ERR;
	}

	int fd = mkstemp(tmp);

	if (fd < 0) {
		free(hl.hashes);
		return CAS_EIO;
	}

	struct cas_pack_index_entry *index =
		calloc((size_t)hl.count, sizeof(*index));

	if (!index) {
		close(fd);
		unlink(tmp);
		free(hl.hashes);
		return CAS_ENOMEM;
	}

	uint64_t offset = 0;

	for (int i = 0; i < hl.count; i++) {
		char hex[CAS_HASH_HEX + 1];

		cas_hex_encode(hl.hashes[i], CAS_HASH_LEN, hex);

		struct cas_file cf;
		char type[CAS_TYPE_MAX + 1];
		int rc = cas_open_object(store, &cf, hex,
		                         type, sizeof(type));

		if (rc != CAS_OK) {
			close(fd);
			unlink(tmp);
			free(index);
			free(hl.hashes);
			return rc;
		}

		if (cf.len > 0) {
			rc = write_full(fd, cf.data, cf.len);
			if (rc != CAS_OK) {
				cas_close(&cf);
				close(fd);
				unlink(tmp);
				free(index);
				free(hl.hashes);
				return rc;
			}
		}
		offset += cf.len;

		struct cas_pack_trailer tr;
		static const unsigned char magic[] =
			CAS_PACK_TRAILER_MAGIC;

		memcpy(tr.magic, magic, CAS_PACK_MAGIC_LEN);
		memset(tr.header, 0, CAS_PACK_HEADER_LEN);

		char hdr[32];
		int hdrlen = format_header(hdr, sizeof(hdr),
		                           type, cf.len);

		cas_close(&cf);

		if (hdrlen < 0 ||
		    (size_t)hdrlen > CAS_PACK_HEADER_LEN) {
			close(fd);
			unlink(tmp);
			free(index);
			free(hl.hashes);
			return CAS_ERR;
		}

		memcpy(tr.header, hdr, (size_t)hdrlen);

		rc = write_full(fd, &tr, sizeof(tr));
		if (rc != CAS_OK) {
			close(fd);
			unlink(tmp);
			free(index);
			free(hl.hashes);
			return rc;
		}

		memcpy(index[i].hash, hl.hashes[i], CAS_HASH_LEN);
		index[i].offset = offset;

		offset += CAS_PACK_BLOCK;
	}

	free(hl.hashes);

	size_t index_size = (size_t)hl.count * sizeof(*index);
	int rc = write_full(fd, index, index_size);

	if (rc != CAS_OK) {
		close(fd);
		unlink(tmp);
		free(index);
		return rc;
	}

	struct cas_pack_footer ft;
	static const unsigned char footer_magic[] =
		CAS_PACK_FOOTER_MAGIC_V1;

	memcpy(ft.magic, footer_magic, CAS_PACK_MAGIC_LEN);
	ft.entry_count = (uint64_t)hl.count;
	memset(ft.reserved, 0, sizeof(ft.reserved));
	memset(ft.checksum, 0, sizeof(ft.checksum));

	size_t check_len = index_size + 16;
	unsigned char *check_buf = malloc(check_len);

	if (!check_buf) {
		close(fd);
		unlink(tmp);
		free(index);
		return CAS_ENOMEM;
	}

	memcpy(check_buf, index, index_size);
	memcpy(check_buf + index_size, ft.magic,
	       CAS_PACK_MAGIC_LEN);
	memcpy(check_buf + index_size + CAS_PACK_MAGIC_LEN,
	       &ft.entry_count, sizeof(ft.entry_count));

	cas_digest(check_buf, check_len, ft.checksum);
	free(check_buf);
	free(index);

	rc = write_full(fd, &ft, sizeof(ft));
	if (rc != CAS_OK) {
		close(fd);
		unlink(tmp);
		return rc;
	}

	close(fd);

	if (rename(tmp, path) != 0) {
		unlink(tmp);
		return CAS_EIO;
	}

	return CAS_OK;
}
