// SPDX-License-Identifier: Apache-2.0
// Fuzz-lite over every test vector: random byte flips and stamped runs (200 mutants per file: a
// third on the headers, a third anywhere, a third on the payload), plus truncations. A corrupt file
// may be refused or decode to garbage, but it must never crash, hang, write outside its buffers,
// produce runaway output, return more frames than asked, or leak memory.
//
// Every mutant is reproducible on its own. Environment knobs for local digging:
//   FUZZ_ITERATIONS=<n>  mutants per file (default 200)
//   FUZZ_FILE=<text>     only files whose name contains <text>
//   FUZZ_ONLY=<i>        only mutant number <i>
//   FUZZ_TRACE=1         print each mutant before running it
//   FUZZ_DUMP=<path>     write the selected mutant(s) to <path> before running them
#include <stdlib.h>

#include "audio_player/decoder.h"
#include "test.h"

#define ITERATIONS 200
#define MAX_SEEKS 3

// ------------------------------------------------------------------ rng ----
static uint32_t xorshift(uint32_t *s) {
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

static uint32_t hash_name(const char *name, uint32_t salt) {
    uint32_t h = 0x9E3779B9u ^ salt;
    for (const char *p = name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return h ? h : 1;
}

// ----------------------------------------------------------- allocator ----
// Guarded allocator: counts live blocks, surrounds every block with canaries and keeps freed
// blocks in a quarantine, so overflows and double frees are caught right where they happen even
// without AddressSanitizer.
#define GUARD 32
#define QUARANTINE 256
#define LIVE_MAGIC 0xA5u
#define FREED_MAGIC 0xDDu
static long g_live;
static const char *g_current = "";
static void *g_quarantine[QUARANTINE];
static size_t g_q_next;

static void die(const char *what, size_t n) {
    printf("  %s (%zu-byte block) while fuzzing %s\n", what, n, g_current);
    fflush(stdout);
    abort();
}

static void *guarded_alloc(size_t n) {
    uint8_t *p = malloc(n + 3 * GUARD);
    if (!p) return NULL;
    memcpy(p, &n, sizeof n);
    memset(p + sizeof n, LIVE_MAGIC, 2 * GUARD - sizeof n);
    memset(p + 2 * GUARD + n, 0x5A, GUARD);
    g_live++;
    return p + 2 * GUARD;
}

static size_t check_block(uint8_t *user) {
    uint8_t *p = user - 2 * GUARD;
    size_t n;
    memcpy(&n, p, sizeof n);
    if (p[sizeof n] == FREED_MAGIC) die("double free", n);
    for (size_t i = sizeof n; i < 2 * GUARD; i++) {
        if (p[i] != LIVE_MAGIC) die("write before the start of a block", n);
    }
    for (size_t i = 0; i < GUARD; i++) {
        if (user[n + i] != 0x5A) die("write past the end of a block", n);
    }
    return n;
}

static void guarded_free(void *ptr) {
    if (!ptr) return;
    size_t n = check_block(ptr);
    uint8_t *p = (uint8_t *)ptr - 2 * GUARD;
    memset(p + sizeof n, FREED_MAGIC, 2 * GUARD - sizeof n);
    memset(ptr, 0xEE, n);  // stale reads see garbage, not the old data
    g_live--;
    free(g_quarantine[g_q_next]);
    g_quarantine[g_q_next] = p;
    g_q_next = (g_q_next + 1) % QUARANTINE;
}

static void *guarded_realloc(void *ptr, size_t n) {
    if (!ptr) return guarded_alloc(n);
    size_t old = check_block(ptr);
    void *q = guarded_alloc(n);
    if (!q) return NULL;
    memcpy(q, ptr, CORE_MIN(old, n));
    guarded_free(ptr);
    return q;
}

static const core_allocator_t k_guarded = {guarded_alloc, guarded_alloc, guarded_realloc, guarded_free};

static void quarantine_flush(void) {
    for (size_t i = 0; i < QUARANTINE; i++) {
        free(g_quarantine[i]);
        g_quarantine[i] = NULL;
    }
}

// --------------------------------------------------------------- helpers ----
static uint8_t *load(const char *name, size_t *len) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", TEST_DATA_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = n > 0 ? malloc((size_t)n) : NULL;
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        p = NULL;
    }
    fclose(f);
    *len = p ? (size_t)n : 0;
    return p;
}

typedef struct {
    int opened, rejected, read_errors;
    uint64_t frames;
} stats_t;

// Opens and decodes one mutant to the end with random read sizes and a few random seeks.
// The output buffer is heap-allocated with canaries too, so writing past it is caught.
static void run_mutant(const uint8_t *data, size_t len, const char *name, uint64_t cap, uint32_t seed, stats_t *st) {
    enum { MAX_READ = 2048 };
    int32_t *out = guarded_alloc(sizeof(int32_t) * AUDIO_MAX_CHANNELS * MAX_READ);
    if (!out) return;
    long live_before = g_live;
    int err = 0;
    decoder_info_t info;
    decoder_t *d = decoder_open(core_stream_open_memory(data, len, false), name, &info, &err);
    if (!d) {
        st->rejected++;
        g_test_checks++;
        if (err >= 0) TEST_FAIL_("%s: open failed without an error code", name);
    } else {
        st->opened++;
        uint64_t total = 0;
        int seeks = 0;
        for (;;) {
            uint32_t want = 1 + xorshift(&seed) % MAX_READ;
            int32_t n = decoder_read(d, out, want);
            if (n < 0) {
                st->read_errors++;
                break;
            }
            if (n == 0) break;
            if ((uint32_t)n > want) {
                TEST_FAIL_("%s: read returned %d frames for %u", name, n, want);
                break;
            }
            total += (uint64_t)n;
            if (total > cap) {
                TEST_FAIL_("%s: runaway output (%llu frames)", name, (unsigned long long)total);
                break;
            }
            if (info.seekable && seeks < MAX_SEEKS && xorshift(&seed) % 32 == 0) {
                seeks++;
                uint64_t span = info.total_frames ? info.total_frames + 1 : 1u << 20;
                decoder_seek(d, xorshift(&seed) % span);  // may fail on garbage; must not break the decoder
            }
        }
        st->frames += total;
        decoder_close(d);
    }
    g_test_checks++;
    if (g_live != live_before) TEST_FAIL_("%s: %ld blocks leaked", name, g_live - live_before);
    guarded_free(out);
}

static void mutate(uint8_t *buf, size_t len, int zone, uint32_t *rng) {
    int edits = 1 + (int)(xorshift(rng) % 8);
    for (int e = 0; e < edits; e++) {
        size_t pos = zone == 0 ? xorshift(rng) % CORE_MIN(len, (size_t)4096)
                     : zone == 1 ? xorshift(rng) % len
                                 : len / 2 + xorshift(rng) % (len - len / 2);
        switch (xorshift(rng) % 4) {
        case 0: buf[pos] ^= (uint8_t)(1u << (xorshift(rng) % 8)); break;  // single bit flip
        case 1: buf[pos] = (uint8_t)xorshift(rng); break;                  // random byte
        case 2: buf[pos] = (xorshift(rng) & 1) ? 0xFF : 0x00; break;       // extreme byte
        default: {                                                         // stamped run
            size_t run = 1 + xorshift(rng) % 64;  // not inside CORE_MIN: it evaluates arguments twice
            size_t end = CORE_MIN(len, pos + run);
            uint8_t v = (xorshift(rng) & 1) ? 0xFF : 0x00;
            for (size_t k = pos; k < end; k++) buf[k] = v;
            break;
        }
        }
    }
}

static void fuzz_file(const char *name, uint64_t frames) {
    const char *only_file = getenv("FUZZ_FILE");
    if (only_file && !strstr(name, only_file)) return;
    const char *env = getenv("FUZZ_ITERATIONS");
    int iterations = env && atoi(env) > 0 ? atoi(env) : ITERATIONS;
    int only = getenv("FUZZ_ONLY") ? atoi(getenv("FUZZ_ONLY")) : -1;
    int from = getenv("FUZZ_FROM") ? atoi(getenv("FUZZ_FROM")) : 0;
    bool trace = getenv("FUZZ_TRACE") != NULL;
    const char *dump = getenv("FUZZ_DUMP");

    g_current = name;
    size_t len = 0;
    uint8_t *orig = load(name, &len);
    g_test_checks++;
    if (!orig) {
        TEST_FAIL_("%s: cannot load", name);
        return;
    }
    uint8_t *buf = malloc(len);
    if (!buf) {
        free(orig);
        return;
    }
    uint64_t cap = frames * (MAX_SEEKS + 1) * 4 + 1000000;
    stats_t st = {0};
    for (int it = 0; it < iterations; it++) {
        if ((only >= 0 && it != only) || it < from) continue;
        uint32_t rng = hash_name(name, (uint32_t)it);
        memcpy(buf, orig, len);
        mutate(buf, len, it % 3, &rng);
        size_t n = xorshift(&rng) % 8 == 0 ? xorshift(&rng) % (len + 1) : len;  // sometimes cut short too
        if (trace) printf("  %s #%d (%zu bytes)\n", name, it, n);
        if (dump) {
            FILE *f = fopen(dump, "wb");
            if (f) {
                fwrite(buf, 1, n, f);
                fclose(f);
            }
        }
        run_mutant(buf, n, name, cap, rng, &st);
    }
    // Clean truncations at interesting lengths.
    static const size_t k_cuts[] = {0,  1,  3,  4,  8,  11,  12,  16,  27,  28,  36,
                                    42, 44, 58, 64, 92, 100, 128, 256, 512, 4096};
    for (size_t i = 0; only < 0 && i < CORE_ARRAY_SIZE(k_cuts) + 3; i++) {
        size_t cut = i < CORE_ARRAY_SIZE(k_cuts) ? k_cuts[i] : len * (i - CORE_ARRAY_SIZE(k_cuts) + 1) / 4;
        if (cut > len) continue;
        memcpy(buf, orig, cut);
        run_mutant(buf, cut, name, cap, hash_name(name, 0xC07u + (uint32_t)i), &st);
    }
    printf("  %-26s opened %4d, rejected %4d, read errors %4d, %9llu frames\n", name, st.opened, st.rejected,
           st.read_errors, (unsigned long long)st.frames);
    free(buf);
    free(orig);
}

// ------------------------------------------------------------------ tests ----
TEST(fuzz_all_vectors) {
    char path[512];
    snprintf(path, sizeof path, "%s/vectors.txt", TEST_DATA_DIR);
    FILE *f = fopen(path, "r");
    CHECK(f != NULL);
    if (!f) return;
    setvbuf(stdout, NULL, _IONBF, 0);  // keep the progress lines if a mutant takes the process down
    core_set_allocator(&k_guarded);
    core_set_log_sink(NULL, CORE_LOG_ERROR);  // corrupt files log warnings by design
    char line[512];
    int files = 0;
    while (fgets(line, sizeof line, f)) {
        char name[128];
        unsigned long long frames = 0;
        if (line[0] == '#' || sscanf(line, "%127s %*s %*u %*u %*u %*u %llu", name, &frames) != 2) continue;
        fuzz_file(name, frames);
        files++;
    }
    fclose(f);
    core_set_log_sink(NULL, CORE_LOG_INFO);
    core_set_allocator(NULL);
    quarantine_flush();
    CHECK(files >= 20);
}

TEST_MAIN(RUN(fuzz_all_vectors))
