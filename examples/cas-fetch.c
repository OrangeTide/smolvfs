/* cas-fetch.c : reference incremental downloader for smolvfs depots.
 *
 * Fetches a snapshot from a static HTTP origin into a local depot,
 * downloading only the objects the depot does not already hold.  It
 * implements the loose-object transport from DOWNLOAD.md: fetch the
 * ref, then walk the tree from its root, GETting each reachable object
 * that is missing locally and verifying it against its address before
 * storing it.  Re-running against a newer ref fetches only the changed
 * objects.
 *
 * This is a worked example, not part of the library.  It links against
 * the smolvfs objects for verification and storage and against libcurl
 * for HTTP.  Build compressed-depot support with MINIZ=1.
 *
 * Usage: cas-fetch <base-url> <ref-name> <depot-dir>
 *   e.g. cas-fetch https://cdn.example/depot main ./cache
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#define _POSIX_C_SOURCE 200809L

#include "cas.h"
#include "cas-tree.h"

#include <curl/curl.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_URL  8192
#define MAX_PATH 4096

/****************************************************************
 * Growable response buffer
 ****************************************************************/

struct buf {
    char   *data;
    size_t  len;
    size_t  cap;
};

static size_t
write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct buf *b = userdata;
    size_t add = size * nmemb;

    if (b->len + add > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;

        while (cap < b->len + add)
            cap *= 2;

        char *p = realloc(b->data, cap);

        if (!p)
            return 0;  /* signals an error to libcurl */
        b->data = p;
        b->cap = cap;
    }

    memcpy(b->data + b->len, ptr, add);
    b->len += add;
    return add;
}

/****************************************************************
 * HTTP GET
 ****************************************************************/

/* Fetch url into b (reset first).  Returns 0 on a 2xx response, -1
 * otherwise.  The handle is reused across calls for keep-alive. */
static int
http_get(CURL *curl, const char *url, struct buf *b)
{
    b->len = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, b);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cas-fetch/1");

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        long code = 0;

        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        fprintf(stderr, "GET %s: %s (HTTP %ld)\n", url,
                curl_easy_strerror(res), code);
        return -1;
    }
    return 0;
}

/****************************************************************
 * Storing a fetched loose object
 ****************************************************************/

static int
write_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, data, len);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        data += n;
        len -= (size_t)n;
    }
    return 0;
}

/* The fetched bytes are already the loose-object file (data region plus
 * trailer), so write them verbatim into <depot>/<xx>/<hash> and let the
 * library verify the result against its address. */
static int
store_object(struct cas *store, const char *base, const char *hash,
             const struct buf *b)
{
    char dir[MAX_PATH], path[MAX_PATH], tmp[MAX_PATH];

    if (snprintf(dir, sizeof(dir), "%s/%.2s", base, hash) >=
            (int)sizeof(dir) ||
        snprintf(path, sizeof(path), "%s/%s", dir, hash) >=
            (int)sizeof(path) ||
        snprintf(tmp, sizeof(tmp), "%s/.fetch.XXXXXX", dir) >=
            (int)sizeof(tmp)) {
        fprintf(stderr, "path too long for %s\n", hash);
        return -1;
    }

    mkdir(base, 0755);
    mkdir(dir, 0755);

    int fd = mkstemp(tmp);

    if (fd < 0) {
        fprintf(stderr, "mkstemp %s: %s\n", tmp, strerror(errno));
        return -1;
    }

    if (write_all(fd, b->data, b->len) != 0) {
        fprintf(stderr, "write %s: %s\n", tmp, strerror(errno));
        close(fd);
        unlink(tmp);
        return -1;
    }
    close(fd);

    if (rename(tmp, path) != 0) {
        fprintf(stderr, "rename %s: %s\n", path, strerror(errno));
        unlink(tmp);
        return -1;
    }

    int st = cas_fsck_object(store, hash);

    if (st == CAS_FSCK_OK || st == CAS_FSCK_REENCODED)
        return 0;

    unlink(path);
    if (st == CAS_FSCK_NOCODEC)
        fprintf(stderr, "%s is compressed; rebuild cas-fetch with "
                "MINIZ=1 to verify and read it\n", hash);
    else
        fprintf(stderr, "%s failed verification (status %d); the "
                "origin served corrupt or wrong data\n", hash, st);
    return -1;
}

/****************************************************************
 * Work queue (a stack of pending hashes)
 ****************************************************************/

struct queue {
    char   (*items)[CAS_HASH_HEX + 1];
    size_t   len;
    size_t   cap;
};

static int
queue_push(struct queue *q, const char *hash)
{
    if (q->len == q->cap) {
        size_t cap = q->cap ? q->cap * 2 : 64;
        void *p = realloc(q->items, cap * sizeof(*q->items));

        if (!p)
            return -1;
        q->items = p;
        q->cap = cap;
    }

    memcpy(q->items[q->len], hash, CAS_HASH_HEX + 1);
    q->len++;
    return 0;
}

/****************************************************************
 * Ref parsing
 ****************************************************************/

/* Copy the 64-hex root address out of a ref response into out. */
static int
parse_ref(const struct buf *b, char *out)
{
    size_t i = 0;

    while (i < b->len && isspace((unsigned char)b->data[i]))
        i++;

    if (b->len - i < CAS_HASH_HEX)
        return -1;

    for (size_t j = 0; j < CAS_HASH_HEX; j++)
        if (!isxdigit((unsigned char)b->data[i + j]))
            return -1;

    memcpy(out, b->data + i, CAS_HASH_HEX);
    out[CAS_HASH_HEX] = '\0';
    return 0;
}

/****************************************************************
 * Main
 ****************************************************************/

int
main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr,
                "usage: %s <base-url> <ref-name> <depot-dir>\n",
                argv[0]);
        return 2;
    }

    const char *base = argv[1];
    const char *ref = argv[2];
    const char *depot = argv[3];

    curl_global_init(CURL_GLOBAL_DEFAULT);

    CURL *curl = curl_easy_init();
    struct cas *store = cas_new(depot);
    struct cas_tree *ct = store ? cas_tree_new(store) : NULL;
    struct buf b = {0};
    struct queue q = {0};
    int rc = 1;

    if (!curl || !store || !ct) {
        fprintf(stderr, "initialization failed\n");
        goto out;
    }

    /* discovery: fetch the ref to get the root address */
    char url[MAX_URL];
    char root[CAS_HASH_HEX + 1];

    if (snprintf(url, sizeof(url), "%s/refs/%s.root", base, ref) >=
            (int)sizeof(url)) {
        fprintf(stderr, "url too long\n");
        goto out;
    }
    if (http_get(curl, url, &b) != 0)
        goto out;
    if (parse_ref(&b, root) != 0) {
        fprintf(stderr, "ref '%s' is not a 64-hex address\n", ref);
        goto out;
    }

    if (queue_push(&q, root) != 0)
        goto out;

    /* walk the tree, fetching only what is missing */
    size_t objects = 0, bytes = 0;

    while (q.len > 0) {
        char hash[CAS_HASH_HEX + 1];

        q.len--;
        memcpy(hash, q.items[q.len], sizeof(hash));

        if (cas_exists(store, hash))
            continue;  /* already cached (prior run or shared object) */

        if (snprintf(url, sizeof(url), "%s/%.2s/%s",
                     base, hash, hash) >= (int)sizeof(url)) {
            fprintf(stderr, "url too long\n");
            goto out;
        }
        if (http_get(curl, url, &b) != 0)
            goto out;
        if (store_object(store, cas_basedir(store), hash, &b) != 0)
            goto out;

        objects++;
        bytes += b.len;

        /* if it is a directory, enqueue its children */
        struct cas_file cf;
        char type[CAS_TYPE_MAX + 1];

        if (cas_open_object(store, &cf, hash, type,
                            sizeof(type)) == CAS_OK) {
            cas_close(&cf);

            if (strcmp(type, "tree") == 0 ||
                strcmp(type, "htree") == 0) {
                struct cas_tree_dir dir;

                if (cas_tree_load(ct, hash, &dir) == CAS_OK) {
                    for (int i = 0; i < dir.count; i++)
                        if (queue_push(&q, dir.entries[i].hash) != 0) {
                            cas_tree_dir_free(&dir);
                            goto out;
                        }
                    cas_tree_dir_free(&dir);
                }
            }
        }

        fprintf(stderr, "\rfetched %zu objects, %zu bytes",
                objects, bytes);
    }
    fprintf(stderr, "\n");

    /* record the ref only after every reachable object is present */
    if (cas_tree_ref_commit(ct, ref, root, "downloaded via cas-fetch")
            != CAS_OK) {
        fprintf(stderr, "failed to record ref '%s'\n", ref);
        goto out;
    }

    fprintf(stderr, "ref '%s' -> %s\n", ref, root);
    rc = 0;

out:
    free(q.items);
    free(b.data);
    if (ct)
        cas_tree_free(ct);
    if (store)
        cas_free(store);
    if (curl)
        curl_easy_cleanup(curl);
    curl_global_cleanup();
    return rc;
}
