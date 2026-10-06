// SPDX-License-Identifier: Apache-2.0
// Portable file-system helpers for the library module: directory iteration, stat,
// mkdir, atomic replace, fsync; plus CRC-32 and FNV-1a hashes.
//
// On ESP-IDF the FAT VFS provides dirent/stat; stat() right after readdir() on the
// same entry is served from the directory stream cache, so scanning does not pay a
// second directory search per file. On Windows (host tests) mingw provides
// dirent.h without d_type, so the type comes from stat().
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <dirent.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "lib_priv.h"

// ------------------------------------------------------------------ CRC-32 ----
static uint32_t s_crc_table[256];
static volatile bool s_crc_ready;

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        s_crc_table[i] = c;
    }
    s_crc_ready = true;  // idempotent: a concurrent first call computes the same table
}

uint32_t lib_crc32(uint32_t crc, const void *data, size_t len) {
    if (!s_crc_ready) crc_init();
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) crc = s_crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t lib_fnv1a32(uint32_t h, const void *data, size_t len) {
    const uint8_t *p = data;
    while (len--) {
        h ^= *p++;
        h *= 0x01000193u;
    }
    return h;
}

uint64_t lib_fnv1a64(uint64_t h, const void *data, size_t len) {
    const uint8_t *p = data;
    while (len--) {
        h ^= *p++;
        h *= 0x100000001B3ull;
    }
    return h;
}

uint32_t lib_path_hash(const char *path) { return lib_fnv1a32(LIB_FNV32_INIT, path, strlen(path)); }
uint64_t lib_path_hash64(const char *path) { return lib_fnv1a64(LIB_FNV64_INIT, path, strlen(path)); }

// ------------------------------------------------------------ file system ----
int lfs_stat(const char *path, lfs_stat_t *st) {
    struct stat s;
    if (stat(path, &s) != 0) return errno == ENOENT ? CORE_ENOTFOUND : CORE_EIO;
    st->is_dir = S_ISDIR(s.st_mode);
    st->size = s.st_size > 0 ? (uint64_t)s.st_size : 0;
    st->mtime = s.st_mtime > 0 ? (uint32_t)s.st_mtime : 0;
    return CORE_OK;
}

bool lfs_exists(const char *path) {
    struct stat s;
    return stat(path, &s) == 0;
}

struct lfs_dir {
    DIR *d;
    bool failed;
};

lfs_dir_t *lfs_opendir(const char *path) {
    DIR *d = opendir(path);
    if (!d) return NULL;
    lfs_dir_t *ld = core_malloc_fast(sizeof *ld);
    if (!ld) {
        closedir(d);
        return NULL;
    }
    ld->d = d;
    ld->failed = false;
    return ld;
}

bool lfs_readdir(lfs_dir_t *ld, lfs_dirent_t *e) {
    if (!ld) return false;
    for (;;) {
        errno = 0;
        struct dirent *de = readdir(ld->d);
        if (!de) {
            ld->failed = errno != 0;  // NULL with errno set: an error, not the end
            return false;
        }
        if (de->d_name[0] == '.' && (de->d_name[1] == 0 || (de->d_name[1] == '.' && de->d_name[2] == 0))) continue;
        e->name = de->d_name;
        e->type = LFS_TYPE_UNKNOWN;
#if defined(DT_DIR) && defined(DT_REG)
        if (de->d_type == DT_DIR) e->type = LFS_TYPE_DIR;
        else if (de->d_type == DT_REG) e->type = LFS_TYPE_FILE;
#endif
        return true;
    }
}

bool lfs_dir_failed(const lfs_dir_t *ld) { return ld && ld->failed; }

void lfs_closedir(lfs_dir_t *ld) {
    if (!ld) return;
    closedir(ld->d);
    core_free(ld);
}

int lfs_mkdir(const char *path) {
#if defined(_WIN32)
    int r = _mkdir(path);
#else
    int r = mkdir(path, 0775);
#endif
    if (r == 0) return CORE_OK;
    lfs_stat_t st;
    if (lfs_stat(path, &st) == CORE_OK && st.is_dir) return CORE_OK;
    return CORE_EIO;
}

int lfs_mkdirs(const char *path) {
    char buf[CORE_PATH_MAX];
    if (core_strlcpy(buf, path, sizeof buf) >= sizeof buf) return CORE_EINVAL;
    size_t len = strlen(buf);
    while (len > 1 && buf[len - 1] == '/') buf[--len] = 0;
    for (size_t i = 1; i < len; i++) {
        if (buf[i] != '/') continue;
        buf[i] = 0;
        // Skip drive roots ("C:") and mount points that cannot be created; the final
        // mkdir reports real failures.
        if (!lfs_exists(buf)) lfs_mkdir(buf);
        buf[i] = '/';
    }
    return lfs_mkdir(buf);
}

int lfs_remove(const char *path) {
    if (remove(path) == 0) return CORE_OK;
    return lfs_exists(path) ? CORE_EIO : CORE_OK;
}

int lfs_rmdir(const char *path) {
#if defined(_WIN32)
    int r = _rmdir(path);
#else
    int r = rmdir(path);
#endif
    if (r == 0) return CORE_OK;
    return lfs_exists(path) ? CORE_EIO : CORE_OK;
}

int lfs_replace(const char *from, const char *to) {
    if (rename(from, to) == 0) return CORE_OK;
    // FAT and Windows refuse to overwrite: remove the target first.
    if (lfs_exists(to) && remove(to) != 0) return CORE_EIO;
    return rename(from, to) == 0 ? CORE_OK : CORE_EIO;
}

int lfs_fsync(FILE *f) {
    if (fflush(f) != 0) return CORE_EIO;
#if defined(_WIN32)
    return _commit(_fileno(f)) == 0 ? CORE_OK : CORE_EIO;
#else
    return fsync(fileno(f)) == 0 ? CORE_OK : CORE_EIO;
#endif
}

bool lfs_is_skipped_dir(const char *name) {
    if (!name || !*name || name[0] == '.') return true;
    return strcmp(name, "System Volume Information") == 0 || strcmp(name, "$RECYCLE.BIN") == 0 ||
           strcmp(name, "RECYCLER") == 0 || strcmp(name, "LOST.DIR") == 0;
}

int lfs_write_atomic(const char *path, const void *data, size_t len) {
    char tmp[CORE_PATH_MAX];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp) return CORE_EINVAL;
    FILE *f = fopen(tmp, "wb");
    if (!f) return CORE_EIO;
    int r = CORE_OK;
    if (len && fwrite(data, 1, len, f) != len) r = CORE_EIO;
    if (r == CORE_OK) r = lfs_fsync(f);
    if (fclose(f) != 0 && r == CORE_OK) r = CORE_EIO;
    if (r == CORE_OK) r = lfs_replace(tmp, path);
    if (r != CORE_OK) remove(tmp);
    return r;
}
