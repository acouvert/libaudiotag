/*
 * tests.c — unit / regression tests for libaudiotag.
 *
 * Each test function returns 0 on success, -1 on failure.
 * main() runs them all and exits with the number of failures.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_metadata.h"
#include "audio_stream.h"
#include "flac_metadata.h"
#include "mp3_metadata.h"

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

#define MAX_TAGS 32

typedef struct {
    int   count;
    char* keys[MAX_TAGS];
    char* vals[MAX_TAGS];
} TagBag;

static void collect_tag(void* ctx, const char* key, const char* value)
{
    TagBag* bag = (TagBag*)ctx;
    if (bag->count < MAX_TAGS)
    {
        bag->keys[bag->count] = strdup(key);
        bag->vals[bag->count] = strdup(value);
        bag->count++;
    }
}

static void tag_bag_free(TagBag* bag)
{
    for (int i = 0; i < bag->count; i++)
    {
        free(bag->keys[i]);
        free(bag->vals[i]);
    }
    bag->count = 0;
}

static const char* tag_bag_get(const TagBag* bag, const char* key)
{
    for (int i = 0; i < bag->count; i++)
    {
        if (strcmp(bag->keys[i], key) == 0)
            return bag->vals[i];
    }
    return NULL;
}

/* Memory-backed AudioStream */
typedef struct {
    AudioStream base;
    const uint8_t* data;
    size_t size;
} MemStream;

static int mem_stream_read(const AudioStream* s, size_t offset, size_t length, uint8_t* out)
{
    const MemStream* m = (const MemStream*)s;
    if (offset + length > m->size) return -1;
    memcpy(out, m->data + offset, length);
    return 0;
}

static MemStream mem_stream_create(const uint8_t* data, size_t size)
{
    MemStream m;
    m.base.read = mem_stream_read;
    m.data = data;
    m.size = size;
    return m;
}

/* Memory-backed stream that fails after N successful reads */
typedef struct {
    AudioStream base;
    const uint8_t* data;
    size_t size;
    int reads_left; /* number of successful reads before returning -1 */
    int read_count;
} FailStream;

static int fail_stream_read(const AudioStream* s, size_t offset, size_t length, uint8_t* out)
{
    FailStream* f = (FailStream*)s;
    if (f->read_count >= f->reads_left) return -1;
    f->read_count++;
    if (offset + length > f->size) return -1;
    memcpy(out, f->data + offset, length);
    return 0;
}

static FailStream fail_stream_create(const uint8_t* data, size_t size, int reads_left)
{
    FailStream f;
    f.base.read = fail_stream_read;
    f.data = data;
    f.size = size;
    f.reads_left = reads_left;
    f.read_count = 0;
    return f;
}

/* Write helpers (big-endian / little-endian) */
static void put_be16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_be24(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v; }
static void put_be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put_le32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* Syncsafe encoding for ID3v2 sizes */
static void put_syncsafe32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 21) & 0x7F);
    p[1] = (uint8_t)((v >> 14) & 0x7F);
    p[2] = (uint8_t)((v >>  7) & 0x7F);
    p[3] = (uint8_t)( v        & 0x7F);
}

/* ------------------------------------------------------------------ */
/*  FLAC helpers: build minimal valid FLAC data in a buffer            */
/* ------------------------------------------------------------------ */

/*
 * Build a minimal FLAC stream: fLaC + STREAMINFO + VORBIS_COMMENT.
 * Returns the total size written into buf.
 *
 * sample_rate : Hz
 * total_samples: total decode samples
 * comments    : array of "KEY=value" strings
 * num_comments: length of comments array
 */
static size_t build_flac(uint8_t* buf, size_t bufsize,
                          uint32_t sample_rate, uint64_t total_samples,
                          const char** comments, int num_comments)
{
    (void)bufsize;
    uint8_t* p = buf;

    /* fLaC marker */
    memcpy(p, "fLaC", 4); p += 4;

    /* STREAMINFO metadata block header: type=0, not-last, length=34 */
    *p = 0x00; p++;                    /* type 0, not last */
    put_be24(p, 34); p += 3;

    /* STREAMINFO body (34 bytes) */
    uint8_t* si = p;
    memset(si, 0, 34);
    put_be16(si + 0, 4096);           /* min block size */
    put_be16(si + 2, 4096);           /* max block size */
    /* min/max frame size: 0 (unknown) — already zeroed */

    /* Pack sample_rate(20) | channels-1(3) | bps-1(5) | total_samples(36) into 8 bytes */
    uint64_t composite = 0;
    composite |= ((uint64_t)(sample_rate & 0xFFFFF)) << 44;
    composite |= ((uint64_t)(1))  << 41;  /* channels-1 = 1 → 2ch */
    composite |= ((uint64_t)(15)) << 36;  /* bps-1 = 15 → 16-bit */
    composite |= (total_samples & 0xFFFFFFFFFULL);

    for (int i = 0; i < 8; i++)
        si[10 + i] = (uint8_t)(composite >> (56 - 8 * i));

    /* MD5 left as zeros */
    p += 34;

    /* VORBIS_COMMENT metadata block: type=4, is_last=1 */
    uint8_t* vc_hdr = p;
    p += 4; /* placeholder for header */

    uint8_t* vc_start = p;

    /* vendor string (length + data) */
    const char* vendor = "libaudiotag-test";
    uint32_t vendor_len = (uint32_t)strlen(vendor);
    put_le32(p, vendor_len); p += 4;
    memcpy(p, vendor, vendor_len); p += vendor_len;

    /* comment count */
    put_le32(p, (uint32_t)num_comments); p += 4;

    for (int i = 0; i < num_comments; i++)
    {
        uint32_t clen = (uint32_t)strlen(comments[i]);
        put_le32(p, clen); p += 4;
        memcpy(p, comments[i], clen); p += clen;
    }

    uint32_t vc_len = (uint32_t)(p - vc_start);
    vc_hdr[0] = 0x84; /* type=4, is_last=1 */
    put_be24(vc_hdr + 1, vc_len);

    return (size_t)(p - buf);
}

/* ------------------------------------------------------------------ */
/*  ID3v2 helpers: build minimal ID3v2.3 data in a buffer              */
/* ------------------------------------------------------------------ */

/*
 * Build a minimal ID3v2.3 tag with the given text frames.
 * frame_ids  : array of 4-char frame IDs (e.g. "TIT2")
 * values     : array of corresponding UTF-8 values
 * num_frames : length of arrays
 * Returns total size written.
 */
static size_t build_id3v2(uint8_t* buf, size_t bufsize,
                           const char** frame_ids, const char** values, int num_frames)
{
    (void)bufsize;
    uint8_t* p = buf;

    /* ID3v2 header (10 bytes) — fill size later */
    memcpy(p, "ID3", 3); p += 3;
    *p++ = 3;  /* version major = 2.3 */
    *p++ = 0;  /* version minor */
    *p++ = 0;  /* flags */
    uint8_t* size_pos = p; p += 4; /* placeholder */

    uint8_t* frames_start = p;

    for (int i = 0; i < num_frames; i++)
    {
        uint32_t vlen = (uint32_t)strlen(values[i]);
        uint32_t frame_size = 1 + vlen; /* encoding byte + text */

        memcpy(p, frame_ids[i], 4); p += 4;
        put_be32(p, frame_size);     p += 4;
        *p++ = 0; /* flags high */
        *p++ = 0; /* flags low */
        *p++ = 3; /* encoding = UTF-8 */
        memcpy(p, values[i], vlen); p += vlen;
    }

    uint32_t tag_body_size = (uint32_t)(p - frames_start);
    put_syncsafe32(size_pos, tag_body_size);

    return (size_t)(p - buf);
}

/* ------------------------------------------------------------------ */
/*  Test runner macros                                                  */
/* ------------------------------------------------------------------ */

#define ASSERT_EQ_INT(a, b) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "  FAIL %s:%d: %s == %d, expected %d\n", \
                __FILE__, __LINE__, #a, (int)(a), (int)(b)); \
        return -1; \
    } \
} while (0)

#define ASSERT_EQ_STR(a, b) do { \
    const char* _a = (a); const char* _b = (b); \
    if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) { \
        fprintf(stderr, "  FAIL %s:%d: %s == \"%s\", expected \"%s\"\n", \
                __FILE__, __LINE__, #a, _a ? _a : "(null)", _b ? _b : "(null)"); \
        return -1; \
    } \
} while (0)

#define ASSERT_NULL(a) do { \
    if ((a) != NULL) { \
        fprintf(stderr, "  FAIL %s:%d: %s expected NULL\n", \
                __FILE__, __LINE__, #a); \
        return -1; \
    } \
} while (0)

#define ASSERT_NOT_NULL(a) do { \
    if ((a) == NULL) { \
        fprintf(stderr, "  FAIL %s:%d: %s was NULL\n", \
                __FILE__, __LINE__, #a); \
        return -1; \
    } \
} while (0)

#define RUN_TEST(fn) do { \
    fprintf(stderr, "  %-50s", #fn); \
    if (fn() == 0) { passed++; fprintf(stderr, "OK\n"); } \
    else           { failed++; fprintf(stderr, "\n"); } \
} while (0)

/* ------------------------------------------------------------------ */
/*  Tests: NULL / invalid inputs                                       */
/* ------------------------------------------------------------------ */

static int test_null_stream(void)
{
    ASSERT_EQ_INT(audio_stream_read_metadata(NULL, NULL, collect_tag), -1);
    return 0;
}

static int test_null_callback(void)
{
    uint8_t dummy = 0;
    MemStream m = mem_stream_create(&dummy, 1);
    ASSERT_EQ_INT(audio_stream_read_metadata(&m.base, NULL, NULL), -1);
    return 0;
}

static int test_empty_stream(void)
{
    MemStream m = mem_stream_create(NULL, 0);
    ASSERT_EQ_INT(audio_stream_read_metadata(&m.base, NULL, collect_tag), -1);
    return 0;
}

static int test_short_stream(void)
{
    uint8_t data[] = { 'f', 'L' };
    MemStream m = mem_stream_create(data, sizeof(data));
    ASSERT_EQ_INT(flac_read_metadata(&m.base, NULL, collect_tag), -1);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Tests: FLAC with synthetic data                                    */
/* ------------------------------------------------------------------ */

static int test_flac_basic_tags(void)
{
    const char* comments[] = {
        "TITLE=Hello World",
        "ARTIST=Test Artist",
        "ALBUM=Test Album",
        "TRACKNUMBER=3",
    };
    uint8_t buf[1024];
    size_t len = build_flac(buf, sizeof(buf), 44100, 44100 * 60, comments, 4);

    MemStream m = mem_stream_create(buf, len);
    TagBag bag = {0};
    ASSERT_EQ_INT(flac_read_metadata(&m.base, &bag, collect_tag), 0);

    ASSERT_EQ_STR(tag_bag_get(&bag, "title"), "Hello World");
    ASSERT_EQ_STR(tag_bag_get(&bag, "artist"), "Test Artist");
    ASSERT_EQ_STR(tag_bag_get(&bag, "album"), "Test Album");
    ASSERT_EQ_STR(tag_bag_get(&bag, "tracknumber"), "3");
    ASSERT_NOT_NULL(tag_bag_get(&bag, "duration"));

    tag_bag_free(&bag);
    return 0;
}

static int test_flac_duration_calculation(void)
{
    /* 44100 Hz, 132300 samples = exactly 3000 ms */
    uint8_t buf[1024];
    const char* comments[] = { "TITLE=dur" };
    size_t len = build_flac(buf, sizeof(buf), 44100, 132300, comments, 1);

    MemStream m = mem_stream_create(buf, len);
    TagBag bag = {0};
    ASSERT_EQ_INT(flac_read_metadata(&m.base, &bag, collect_tag), 0);
    ASSERT_EQ_STR(tag_bag_get(&bag, "duration"), "3000");

    tag_bag_free(&bag);
    return 0;
}

static int test_flac_key_lowercase(void)
{
    const char* comments[] = { "ArTiSt=Mixed Case" };
    uint8_t buf[1024];
    size_t len = build_flac(buf, sizeof(buf), 44100, 44100, comments, 1);

    MemStream m = mem_stream_create(buf, len);
    TagBag bag = {0};
    ASSERT_EQ_INT(flac_read_metadata(&m.base, &bag, collect_tag), 0);

    /* Key must be lowercased */
    ASSERT_EQ_STR(tag_bag_get(&bag, "artist"), "Mixed Case");
    ASSERT_NULL(tag_bag_get(&bag, "ArTiSt"));

    tag_bag_free(&bag);
    return 0;
}

static int test_flac_empty_value(void)
{
    const char* comments[] = { "GENRE=" };
    uint8_t buf[1024];
    size_t len = build_flac(buf, sizeof(buf), 44100, 44100, comments, 1);

    MemStream m = mem_stream_create(buf, len);
    TagBag bag = {0};
    ASSERT_EQ_INT(flac_read_metadata(&m.base, &bag, collect_tag), 0);
    ASSERT_EQ_STR(tag_bag_get(&bag, "genre"), "");

    tag_bag_free(&bag);
    return 0;
}

static int test_flac_bad_magic(void)
{
    uint8_t buf[] = { 'O', 'g', 'g', 'S' };
    MemStream m = mem_stream_create(buf, sizeof(buf));
    ASSERT_EQ_INT(flac_read_metadata(&m.base, NULL, collect_tag), -1);
    return 0;
}

static int test_flac_missing_vorbis_comment(void)
{
    /* Build FLAC with only STREAMINFO (no VORBIS_COMMENT) */
    uint8_t buf[128];
    uint8_t* p = buf;

    memcpy(p, "fLaC", 4); p += 4;

    /* STREAMINFO: type=0, is_last=1, length=34 */
    *p = 0x80; p++;
    put_be24(p, 34); p += 3;
    memset(p, 0, 34);

    /* Pack a valid sample rate so STREAMINFO parses */
    uint64_t composite = ((uint64_t)44100 << 44) | ((uint64_t)1 << 41) | ((uint64_t)15 << 36) | 44100;
    for (int i = 0; i < 8; i++)
        p[10 + i] = (uint8_t)(composite >> (56 - 8 * i));
    p += 34;

    MemStream m = mem_stream_create(buf, (size_t)(p - buf));
    TagBag bag = {0};

    /* Succeeds because STREAMINFO was found (VORBIS_COMMENT is optional) */
    ASSERT_EQ_INT(flac_read_metadata(&m.base, &bag, collect_tag), 0);

    /* Duration should have been delivered via callback */
    ASSERT_NOT_NULL(tag_bag_get(&bag, "duration"));

    tag_bag_free(&bag);
    return 0;
}

static int test_flac_truncated_streaminfo(void)
{
    /* STREAMINFO block with length < 34 bytes */
    uint8_t buf[64];
    uint8_t* p = buf;

    memcpy(p, "fLaC", 4); p += 4;

    /* STREAMINFO: type=0, is_last=1, length=10 (too short) */
    *p = 0x80; p++;
    put_be24(p, 10); p += 3;
    memset(p, 0, 10);
    p += 10;

    MemStream m = mem_stream_create(buf, (size_t)(p - buf));
    TagBag bag = {0};
    int ret = flac_read_metadata(&m.base, &bag, collect_tag);
    ASSERT_EQ_INT(ret, -1);

    tag_bag_free(&bag);
    return 0;
}

static int test_flac_truncated_vorbis_comment(void)
{
    /* FLAC with STREAMINFO + a VORBIS_COMMENT too short to hold vendor length.
     * VORBIS_COMMENT parse fails, but STREAMINFO already succeeded,
     * so flac_read_metadata returns 0 (at least one block found). */
    uint8_t buf[128];
    uint8_t* p = buf;

    memcpy(p, "fLaC", 4); p += 4;

    /* STREAMINFO: type=0, not-last, length=34 */
    *p = 0x00; p++;
    put_be24(p, 34); p += 3;
    memset(p, 0, 34);
    uint64_t composite = ((uint64_t)44100 << 44) | ((uint64_t)1 << 41) | ((uint64_t)15 << 36) | 44100;
    for (int i = 0; i < 8; i++)
        p[10 + i] = (uint8_t)(composite >> (56 - 8 * i));
    p += 34;

    /* VORBIS_COMMENT: type=4, is_last=1, length=2 (too short for vendor_length u32) */
    *p = 0x84; p++;
    put_be24(p, 2); p += 3;
    *p++ = 0; *p++ = 0;

    MemStream m = mem_stream_create(buf, (size_t)(p - buf));
    TagBag bag = {0};
    /* Both blocks were "found" so the function returns 0 despite vorbis parse failure */
    int ret = flac_read_metadata(&m.base, &bag, collect_tag);
    ASSERT_EQ_INT(ret, 0);

    tag_bag_free(&bag);
    return 0;
}

static int test_flac_zero_sample_rate(void)
{
    /* STREAMINFO with sample_rate=0 — duration should be skipped */
    const char* comments[] = { "TITLE=zero" };
    uint8_t buf[1024];
    size_t len = build_flac(buf, sizeof(buf), 0, 44100, comments, 1);

    MemStream m = mem_stream_create(buf, len);
    TagBag bag = {0};
    ASSERT_EQ_INT(flac_read_metadata(&m.base, &bag, collect_tag), 0);

    /* No duration tag when sample_rate is 0 */
    ASSERT_NULL(tag_bag_get(&bag, "duration"));
    /* But other tags still present */
    ASSERT_EQ_STR(tag_bag_get(&bag, "title"), "zero");

    tag_bag_free(&bag);
    return 0;
}

static int test_flac_zero_length_vorbis_comment(void)
{
    /* VORBIS_COMMENT block with length=0 (covers read_vorbis_comment_block early return) */
    uint8_t buf[128];
    uint8_t* p = buf;

    memcpy(p, "fLaC", 4); p += 4;

    /* STREAMINFO: type=0, not-last, length=34 */
    *p = 0x00; p++;
    put_be24(p, 34); p += 3;
    memset(p, 0, 34);
    uint64_t composite = ((uint64_t)44100 << 44) | ((uint64_t)1 << 41) | ((uint64_t)15 << 36) | 44100;
    for (int i = 0; i < 8; i++)
        p[10 + i] = (uint8_t)(composite >> (56 - 8 * i));
    p += 34;

    /* VORBIS_COMMENT: type=4, is_last=1, length=0 */
    *p = 0x84; p++;
    put_be24(p, 0); p += 3;

    MemStream m = mem_stream_create(buf, (size_t)(p - buf));
    TagBag bag = {0};
    int ret = flac_read_metadata(&m.base, &bag, collect_tag);
    /* vorbis_comment_block_found=true before parse, but read_vorbis_comment_block fails
     * and breaks out of the loop. Both flags are true so returns 0. */
    ASSERT_EQ_INT(ret, 0);

    tag_bag_free(&bag);
    return 0;
}

static int test_flac_vorbis_oversized_vendor(void)
{
    /* Vorbis comment where vendor_length exceeds the block size */
    uint8_t buf[128];
    uint8_t* p = buf;

    memcpy(p, "fLaC", 4); p += 4;

    /* STREAMINFO: type=0, not-last, length=34 */
    *p = 0x00; p++;
    put_be24(p, 34); p += 3;
    memset(p, 0, 34);
    uint64_t comp = ((uint64_t)44100 << 44) | ((uint64_t)1 << 41) | ((uint64_t)15 << 36) | 44100;
    for (int i = 0; i < 8; i++)
        p[10 + i] = (uint8_t)(comp >> (56 - 8 * i));
    p += 34;

    /* VORBIS_COMMENT: type=4, is_last=1 */
    *p = 0x84; p++;
    uint8_t* vc_len_pos = p; p += 3;

    uint8_t* vc_start = p;
    /* vendor_length = 9999 (way beyond block) */
    put_le32(p, 9999); p += 4;

    uint32_t vc_len = (uint32_t)(p - vc_start);
    put_be24(vc_len_pos, vc_len);

    MemStream m = mem_stream_create(buf, (size_t)(p - buf));
    TagBag bag = {0};
    int ret = flac_read_metadata(&m.base, &bag, collect_tag);
    ASSERT_EQ_INT(ret, 0); /* both blocks "found" */

    tag_bag_free(&bag);
    return 0;
}

static int test_flac_vorbis_truncated_after_vendor(void)
{
    /* Vorbis comment with valid vendor but no room for comment_count */
    uint8_t buf[128];
    uint8_t* p = buf;

    memcpy(p, "fLaC", 4); p += 4;

    /* STREAMINFO: type=0, not-last, length=34 */
    *p = 0x00; p++;
    put_be24(p, 34); p += 3;
    memset(p, 0, 34);
    uint64_t comp = ((uint64_t)44100 << 44) | ((uint64_t)1 << 41) | ((uint64_t)15 << 36) | 44100;
    for (int i = 0; i < 8; i++)
        p[10 + i] = (uint8_t)(comp >> (56 - 8 * i));
    p += 34;

    /* VORBIS_COMMENT: type=4, is_last=1 */
    *p = 0x84; p++;
    uint8_t* vc_len_pos = p; p += 3;

    uint8_t* vc_start = p;
    /* vendor_length = 2, vendor = "ab", then block ends (no comment_count) */
    put_le32(p, 2); p += 4;
    *p++ = 'a'; *p++ = 'b';

    uint32_t vc_len = (uint32_t)(p - vc_start);
    put_be24(vc_len_pos, vc_len);

    MemStream m = mem_stream_create(buf, (size_t)(p - buf));
    TagBag bag = {0};
    int ret = flac_read_metadata(&m.base, &bag, collect_tag);
    ASSERT_EQ_INT(ret, 0);

    tag_bag_free(&bag);
    return 0;
}

static int test_flac_vorbis_oversized_comment(void)
{
    /* Vorbis comment where a comment's length exceeds remaining block */
    uint8_t buf[128];
    uint8_t* p = buf;

    memcpy(p, "fLaC", 4); p += 4;

    /* STREAMINFO: type=0, not-last, length=34 */
    *p = 0x00; p++;
    put_be24(p, 34); p += 3;
    memset(p, 0, 34);
    uint64_t comp = ((uint64_t)44100 << 44) | ((uint64_t)1 << 41) | ((uint64_t)15 << 36) | 44100;
    for (int i = 0; i < 8; i++)
        p[10 + i] = (uint8_t)(comp >> (56 - 8 * i));
    p += 34;

    /* VORBIS_COMMENT: type=4, is_last=1 */
    *p = 0x84; p++;
    uint8_t* vc_len_pos = p; p += 3;

    uint8_t* vc_start = p;
    /* vendor_length = 0 */
    put_le32(p, 0); p += 4;
    /* comment_count = 1 */
    put_le32(p, 1); p += 4;
    /* comment_length = 9999 (way too long) */
    put_le32(p, 9999); p += 4;

    uint32_t vc_len = (uint32_t)(p - vc_start);
    put_be24(vc_len_pos, vc_len);

    MemStream m = mem_stream_create(buf, (size_t)(p - buf));
    TagBag bag = {0};
    int ret = flac_read_metadata(&m.base, &bag, collect_tag);
    ASSERT_EQ_INT(ret, 0);

    tag_bag_free(&bag);
    return 0;
}

static int test_flac_io_error_on_block_header(void)
{
    /* Stream that fails when reading the first metadata block header.
     * read #1 = fLaC marker (OK), read #2 = block header (FAIL) */
    const char* comments[] = { "TITLE=x" };
    uint8_t buf[1024];
    size_t len = build_flac(buf, sizeof(buf), 44100, 44100, comments, 1);

    FailStream fs = fail_stream_create(buf, len, 1);
    int ret = flac_read_metadata(&fs.base, NULL, collect_tag);
    ASSERT_EQ_INT(ret, -1);
    return 0;
}

static int test_flac_io_error_on_streaminfo_read(void)
{
    /* Stream that fails when reading STREAMINFO body.
     * read #1 = fLaC marker (OK), read #2 = block header (OK), read #3 = STREAMINFO body (FAIL) */
    const char* comments[] = { "TITLE=x" };
    uint8_t buf[1024];
    size_t len = build_flac(buf, sizeof(buf), 44100, 44100, comments, 1);

    FailStream fs = fail_stream_create(buf, len, 2);
    int ret = flac_read_metadata(&fs.base, NULL, collect_tag);
    ASSERT_EQ_INT(ret, -1);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Tests: MP3 / ID3v2 with synthetic data                             */
/* ------------------------------------------------------------------ */

static int test_mp3_basic_tags(void)
{
    const char* ids[]  = { "TIT2", "TPE1", "TALB", "TRCK" };
    const char* vals[] = { "Song Title", "Song Artist", "Song Album", "5" };
    uint8_t buf[1024];
    size_t len = build_id3v2(buf, sizeof(buf), ids, vals, 4);

    MemStream m = mem_stream_create(buf, len);
    TagBag bag = {0};
    ASSERT_EQ_INT(mp3_read_metadata(&m.base, &bag, collect_tag), 0);

    ASSERT_EQ_STR(tag_bag_get(&bag, "title"), "Song Title");
    ASSERT_EQ_STR(tag_bag_get(&bag, "artist"), "Song Artist");
    ASSERT_EQ_STR(tag_bag_get(&bag, "album"), "Song Album");
    ASSERT_EQ_STR(tag_bag_get(&bag, "tracknumber"), "5");

    tag_bag_free(&bag);
    return 0;
}

static int test_mp3_date_frames(void)
{
    /* TDRC (v2.4) should map to "date" */
    const char* ids[]  = { "TDRC" };
    const char* vals[] = { "2024" };
    uint8_t buf[256];
    size_t len = build_id3v2(buf, sizeof(buf), ids, vals, 1);

    MemStream m = mem_stream_create(buf, len);
    TagBag bag = {0};
    ASSERT_EQ_INT(mp3_read_metadata(&m.base, &bag, collect_tag), 0);
    ASSERT_EQ_STR(tag_bag_get(&bag, "date"), "2024");

    tag_bag_free(&bag);
    return 0;
}

static int test_mp3_bad_magic(void)
{
    uint8_t buf[] = { 'f', 'L', 'a', 'C' };
    MemStream m = mem_stream_create(buf, sizeof(buf));
    ASSERT_EQ_INT(mp3_read_metadata(&m.base, NULL, collect_tag), -1);
    return 0;
}

static int test_mp3_no_recognized_frames(void)
{
    /* Valid ID3v2 header but only an unknown frame */
    const char* ids[]  = { "ZZZZ" };
    const char* vals[] = { "nothing" };
    uint8_t buf[256];
    size_t len = build_id3v2(buf, sizeof(buf), ids, vals, 1);

    MemStream m = mem_stream_create(buf, len);
    TagBag bag = {0};
    ASSERT_EQ_INT(mp3_read_metadata(&m.base, &bag, collect_tag), -1);
    ASSERT_EQ_INT(bag.count, 0);

    tag_bag_free(&bag);
    return 0;
}

static int test_mp3_duration_frame(void)
{
    const char* ids[]  = { "TLEN" };
    const char* vals[] = { "240000" };
    uint8_t buf[256];
    size_t len = build_id3v2(buf, sizeof(buf), ids, vals, 1);

    MemStream m = mem_stream_create(buf, len);
    TagBag bag = {0};
    ASSERT_EQ_INT(mp3_read_metadata(&m.base, &bag, collect_tag), 0);
    ASSERT_EQ_STR(tag_bag_get(&bag, "duration"), "240000");

    tag_bag_free(&bag);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Tests: MP3 text encodings                                          */
/* ------------------------------------------------------------------ */

/* Helper: build an ID3v2.3 tag with a single raw frame (caller controls encoding + data) */
static size_t build_id3v2_raw_frame(uint8_t* buf, size_t bufsize,
                                     const char* frame_id,
                                     const uint8_t* frame_data, uint32_t frame_data_len)
{
    (void)bufsize;
    uint8_t* p = buf;

    memcpy(p, "ID3", 3); p += 3;
    *p++ = 3;  /* v2.3 */
    *p++ = 0;
    *p++ = 0;  /* flags */
    uint8_t* size_pos = p; p += 4;

    uint8_t* frames_start = p;
    memcpy(p, frame_id, 4); p += 4;
    put_be32(p, frame_data_len); p += 4;
    *p++ = 0; *p++ = 0; /* frame flags */
    memcpy(p, frame_data, frame_data_len); p += frame_data_len;

    put_syncsafe32(size_pos, (uint32_t)(p - frames_start));
    return (size_t)(p - buf);
}

static int test_mp3_encoding_latin1(void)
{
    /* Encoding 0 = Latin-1 */
    uint8_t frame[] = { 0x00, 'H', 'e', 'l', 'l', 'o' };
    uint8_t buf[256];
    size_t len = build_id3v2_raw_frame(buf, sizeof(buf), "TIT2", frame, sizeof(frame));

    MemStream m = mem_stream_create(buf, len);
    TagBag bag = {0};
    ASSERT_EQ_INT(mp3_read_metadata(&m.base, &bag, collect_tag), 0);
    ASSERT_EQ_STR(tag_bag_get(&bag, "title"), "Hello");

    tag_bag_free(&bag);
    return 0;
}

static int test_mp3_encoding_utf16_bom(void)
{
    /* Encoding 1 = UTF-16 with BOM (little-endian) */
    uint8_t frame[] = {
        0x01,                   /* encoding */
        0xFF, 0xFE,             /* BOM (LE) */
        'A', 0x00, 'B', 0x00   /* "AB" in UTF-16LE */
    };
    uint8_t buf[256];
    size_t len = build_id3v2_raw_frame(buf, sizeof(buf), "TIT2", frame, sizeof(frame));

    MemStream m = mem_stream_create(buf, len);
    TagBag bag = {0};
    ASSERT_EQ_INT(mp3_read_metadata(&m.base, &bag, collect_tag), 0);
    ASSERT_EQ_STR(tag_bag_get(&bag, "title"), "AB");

    tag_bag_free(&bag);
    return 0;
}

static int test_mp3_encoding_utf16be(void)
{
    /* Encoding 2 = UTF-16BE (no BOM) */
    uint8_t frame[] = {
        0x02,                   /* encoding */
        0x00, 'X', 0x00, 'Y'   /* "XY" in UTF-16BE */
    };
    uint8_t buf[256];
    size_t len = build_id3v2_raw_frame(buf, sizeof(buf), "TIT2", frame, sizeof(frame));

    MemStream m = mem_stream_create(buf, len);
    TagBag bag = {0};
    ASSERT_EQ_INT(mp3_read_metadata(&m.base, &bag, collect_tag), 0);
    ASSERT_EQ_STR(tag_bag_get(&bag, "title"), "XY");

    tag_bag_free(&bag);
    return 0;
}

static int test_mp3_encoding_latin1_to_utf8(void)
{
    /* Encoding 0 with bytes > 0x7F: Latin-1 0xE9 (é) → UTF-8 \xC3\xA9 */
    uint8_t frame[] = { 0x00, 'B', 'e', 'y', 'o', 'n', 'c', 0xE9 };
    uint8_t buf[256];
    size_t len = build_id3v2_raw_frame(buf, sizeof(buf), "TPE1", frame, sizeof(frame));

    MemStream m = mem_stream_create(buf, len);
    TagBag bag = {0};
    ASSERT_EQ_INT(mp3_read_metadata(&m.base, &bag, collect_tag), 0);
    ASSERT_EQ_STR(tag_bag_get(&bag, "artist"), "Beyonc\xC3\xA9");

    tag_bag_free(&bag);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Tests: MP3 ID3v2.2 (3-byte frames)                                */
/* ------------------------------------------------------------------ */

static int test_mp3_id3v22(void)
{
    /* Build a minimal ID3v2.2 tag by hand: 3-byte frame IDs, 3-byte sizes */
    uint8_t buf[256];
    uint8_t* p = buf;

    memcpy(p, "ID3", 3); p += 3;
    *p++ = 2;  /* version major = 2.2 */
    *p++ = 0;  /* version minor */
    *p++ = 0;  /* flags */
    uint8_t* size_pos = p; p += 4;

    uint8_t* frames_start = p;

    /* TT2 frame = "title" in ID3v2.2 — but our mapping only has 4-char IDs
       so this should NOT be recognized. We verify -1 return. */
    const char* text = "Hello";
    uint32_t frame_size = 1 + (uint32_t)strlen(text);
    memcpy(p, "TT2", 3); p += 3;
    put_be24(p, frame_size); p += 3;
    *p++ = 3; /* UTF-8 */
    memcpy(p, text, strlen(text)); p += strlen(text);

    put_syncsafe32(size_pos, (uint32_t)(p - frames_start));

    MemStream m = mem_stream_create(buf, (size_t)(p - buf));
    TagBag bag = {0};
    /* v2.2 3-byte frames won't match our 4-byte mapping, expect failure */
    ASSERT_EQ_INT(mp3_read_metadata(&m.base, &bag, collect_tag), -1);

    tag_bag_free(&bag);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Tests: MP3 ID3v2.4 syncsafe frame sizes                           */
/* ------------------------------------------------------------------ */

static int test_mp3_id3v24_syncsafe(void)
{
    /* Build an ID3v2.4 tag — frame sizes are syncsafe, not be32 */
    uint8_t buf[256];
    uint8_t* p = buf;

    memcpy(p, "ID3", 3); p += 3;
    *p++ = 4;  /* version major = 2.4 */
    *p++ = 0;
    *p++ = 0;  /* flags */
    uint8_t* size_pos = p; p += 4;

    uint8_t* frames_start = p;

    const char* text = "v2.4 Title";
    uint32_t frame_size = 1 + (uint32_t)strlen(text);

    memcpy(p, "TIT2", 4); p += 4;
    put_syncsafe32(p, frame_size); p += 4; /* syncsafe for v2.4 */
    *p++ = 0; *p++ = 0; /* frame flags */
    *p++ = 3; /* UTF-8 */
    memcpy(p, text, strlen(text)); p += strlen(text);

    put_syncsafe32(size_pos, (uint32_t)(p - frames_start));

    MemStream m = mem_stream_create(buf, (size_t)(p - buf));
    TagBag bag = {0};
    ASSERT_EQ_INT(mp3_read_metadata(&m.base, &bag, collect_tag), 0);
    ASSERT_EQ_STR(tag_bag_get(&bag, "title"), "v2.4 Title");

    tag_bag_free(&bag);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Tests: MP3 extended header                                         */
/* ------------------------------------------------------------------ */

static int test_mp3_extended_header(void)
{
    /* ID3v2.3 with extended header flag set */
    uint8_t buf[256];
    uint8_t* p = buf;

    memcpy(p, "ID3", 3); p += 3;
    *p++ = 3;  /* v2.3 */
    *p++ = 0;
    *p++ = 0x40; /* flags: extended header present */
    uint8_t* size_pos = p; p += 4;

    uint8_t* body_start = p;

    /* Extended header: 4 bytes size (be32 for v2.3), then padding */
    put_be32(p, 6); p += 4; /* ext header size = 6 (includes these 4 bytes + 2 padding) */
    *p++ = 0; *p++ = 0;     /* ext header data */

    /* Now a normal TIT2 frame */
    const char* text = "ExtHdr";
    uint32_t frame_size = 1 + (uint32_t)strlen(text);
    memcpy(p, "TIT2", 4); p += 4;
    put_be32(p, frame_size); p += 4;
    *p++ = 0; *p++ = 0;
    *p++ = 3;
    memcpy(p, text, strlen(text)); p += strlen(text);

    put_syncsafe32(size_pos, (uint32_t)(p - body_start));

    MemStream m = mem_stream_create(buf, (size_t)(p - buf));
    TagBag bag = {0};
    ASSERT_EQ_INT(mp3_read_metadata(&m.base, &bag, collect_tag), 0);
    ASSERT_EQ_STR(tag_bag_get(&bag, "title"), "ExtHdr");

    tag_bag_free(&bag);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Tests: MP3 large text frame (>512 bytes, malloc path)              */
/* ------------------------------------------------------------------ */

static int test_mp3_large_text_frame(void)
{
    /* Value > 512 bytes to exercise the malloc fallback in emit_text_frame */
    char big_value[600];
    memset(big_value, 'A', sizeof(big_value) - 1);
    big_value[sizeof(big_value) - 1] = '\0';

    /* Build raw frame: encoding byte + text */
    uint8_t frame[604];
    frame[0] = 3; /* UTF-8 */
    memcpy(frame + 1, big_value, 599);
    uint32_t frame_len = 1 + 599;

    uint8_t buf[1024];
    size_t len = build_id3v2_raw_frame(buf, sizeof(buf), "TIT2", frame, frame_len);

    MemStream m = mem_stream_create(buf, len);
    TagBag bag = {0};
    ASSERT_EQ_INT(mp3_read_metadata(&m.base, &bag, collect_tag), 0);
    ASSERT_EQ_STR(tag_bag_get(&bag, "title"), big_value);

    tag_bag_free(&bag);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Tests: MP3 error paths and edge cases                              */
/* ------------------------------------------------------------------ */

static int test_mp3_unsupported_version(void)
{
    /* ID3v2 version 5 — unsupported */
    uint8_t buf[16];
    uint8_t* p = buf;
    memcpy(p, "ID3", 3); p += 3;
    *p++ = 5;  /* version major = 5 (unsupported) */
    *p++ = 0;
    *p++ = 0;
    put_syncsafe32(p, 0); p += 4;

    MemStream m = mem_stream_create(buf, (size_t)(p - buf));
    ASSERT_EQ_INT(mp3_read_metadata(&m.base, NULL, collect_tag), -1);
    return 0;
}

static int test_mp3_latin1_trailing_nuls(void)
{
    /* Encoding 0 (Latin-1) with trailing NUL chars that should be trimmed */
    uint8_t frame[] = { 0x00, 'T', 'e', 's', 't', 0x00, 0x00 };
    uint8_t buf[256];
    size_t len = build_id3v2_raw_frame(buf, sizeof(buf), "TIT2", frame, sizeof(frame));

    MemStream m = mem_stream_create(buf, len);
    TagBag bag = {0};
    ASSERT_EQ_INT(mp3_read_metadata(&m.base, &bag, collect_tag), 0);
    ASSERT_EQ_STR(tag_bag_get(&bag, "title"), "Test");

    tag_bag_free(&bag);
    return 0;
}

static int test_mp3_id3v24_extended_header(void)
{
    /* ID3v2.4 with extended header — ext header size is syncsafe */
    uint8_t buf[256];
    uint8_t* p = buf;

    memcpy(p, "ID3", 3); p += 3;
    *p++ = 4;  /* v2.4 */
    *p++ = 0;
    *p++ = 0x40; /* flags: extended header present */
    uint8_t* size_pos = p; p += 4;

    uint8_t* body_start = p;

    /* Extended header: syncsafe size for v2.4 (size includes itself) */
    put_syncsafe32(p, 6); p += 4; /* 6 bytes total ext header */
    *p++ = 0; *p++ = 0;           /* ext header body */

    /* TIT2 frame with syncsafe size for v2.4 */
    const char* text = "v24ext";
    uint32_t frame_size = 1 + (uint32_t)strlen(text);
    memcpy(p, "TIT2", 4); p += 4;
    put_syncsafe32(p, frame_size); p += 4;
    *p++ = 0; *p++ = 0;
    *p++ = 3;
    memcpy(p, text, strlen(text)); p += strlen(text);

    put_syncsafe32(size_pos, (uint32_t)(p - body_start));

    MemStream m = mem_stream_create(buf, (size_t)(p - buf));
    TagBag bag = {0};
    ASSERT_EQ_INT(mp3_read_metadata(&m.base, &bag, collect_tag), 0);
    ASSERT_EQ_STR(tag_bag_get(&bag, "title"), "v24ext");

    tag_bag_free(&bag);
    return 0;
}

static int test_mp3_padding_after_frames(void)
{
    /* Tag with a valid frame followed by NUL padding bytes */
    uint8_t buf[256];
    uint8_t* p = buf;

    memcpy(p, "ID3", 3); p += 3;
    *p++ = 3;  /* v2.3 */
    *p++ = 0;
    *p++ = 0;
    uint8_t* size_pos = p; p += 4;

    uint8_t* frames_start = p;

    /* One valid TIT2 frame */
    const char* text = "Padded";
    uint32_t frame_size = 1 + (uint32_t)strlen(text);
    memcpy(p, "TIT2", 4); p += 4;
    put_be32(p, frame_size); p += 4;
    *p++ = 0; *p++ = 0;
    *p++ = 3;
    memcpy(p, text, strlen(text)); p += strlen(text);

    /* 16 bytes of NUL padding */
    memset(p, 0, 16); p += 16;

    put_syncsafe32(size_pos, (uint32_t)(p - frames_start));

    MemStream m = mem_stream_create(buf, (size_t)(p - buf));
    TagBag bag = {0};
    ASSERT_EQ_INT(mp3_read_metadata(&m.base, &bag, collect_tag), 0);
    ASSERT_EQ_STR(tag_bag_get(&bag, "title"), "Padded");

    tag_bag_free(&bag);
    return 0;
}

static int test_mp3_io_error_on_frame_data(void)
{
    /* Stream that fails when reading frame data.
     * read #1 = ID3v2 header (OK), read #2 = frame header (OK), read #3 = frame data (FAIL) */
    const char* ids[]  = { "TIT2" };
    const char* vals[] = { "fail" };
    uint8_t buf[256];
    size_t len = build_id3v2(buf, sizeof(buf), ids, vals, 1);

    FailStream fs = fail_stream_create(buf, len, 2);
    ASSERT_EQ_INT(mp3_read_metadata(&fs.base, NULL, collect_tag), -1);
    return 0;
}

static int test_mp3_bad_magic_long(void)
{
    /* 10+ bytes but not starting with "ID3" — covers magic check after successful read */
    uint8_t buf[16] = { 'N', 'O', 'T', 'I', 'D', '3', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    MemStream m = mem_stream_create(buf, sizeof(buf));
    ASSERT_EQ_INT(mp3_read_metadata(&m.base, NULL, collect_tag), -1);
    return 0;
}

static int test_mp3_utf8_trailing_nuls(void)
{
    /* UTF-8 frame (encoding 3) with trailing NUL characters — covers NUL trimming */
    uint8_t frame[] = { 0x03, 'H', 'i', 0x00, 0x00 };
    uint8_t buf[256];
    size_t len = build_id3v2_raw_frame(buf, sizeof(buf), "TIT2", frame, sizeof(frame));

    MemStream m = mem_stream_create(buf, len);
    TagBag bag = {0};
    ASSERT_EQ_INT(mp3_read_metadata(&m.base, &bag, collect_tag), 0);
    ASSERT_EQ_STR(tag_bag_get(&bag, "title"), "Hi");

    tag_bag_free(&bag);
    return 0;
}

static int test_mp3_io_error_on_ext_header(void)
{
    /* ID3v2.3 with extended header flag set — I/O error reading the ext header size */
    uint8_t buf[64];
    uint8_t* p = buf;

    memcpy(p, "ID3", 3); p += 3;
    *p++ = 3;  /* v2.3 */
    *p++ = 0;
    *p++ = 0x40;  /* flags: extended header present */
    put_syncsafe32(p, 20); p += 4;
    /* Don't write any ext header data — stream ends here */

    MemStream m = mem_stream_create(buf, 10); /* only the 10-byte header */
    ASSERT_EQ_INT(mp3_read_metadata(&m.base, NULL, collect_tag), -1);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Tests: auto-detect (audio_stream_read_metadata)                    */
/* ------------------------------------------------------------------ */

static int test_autodetect_flac(void)
{
    const char* comments[] = { "TITLE=Auto FLAC" };
    uint8_t buf[1024];
    size_t len = build_flac(buf, sizeof(buf), 48000, 48000, comments, 1);

    MemStream m = mem_stream_create(buf, len);
    TagBag bag = {0};
    ASSERT_EQ_INT(audio_stream_read_metadata(&m.base, &bag, collect_tag), 0);
    ASSERT_EQ_STR(tag_bag_get(&bag, "title"), "Auto FLAC");

    tag_bag_free(&bag);
    return 0;
}

static int test_autodetect_mp3(void)
{
    const char* ids[]  = { "TIT2" };
    const char* vals[] = { "Auto MP3" };
    uint8_t buf[256];
    size_t len = build_id3v2(buf, sizeof(buf), ids, vals, 1);

    MemStream m = mem_stream_create(buf, len);
    TagBag bag = {0};
    ASSERT_EQ_INT(audio_stream_read_metadata(&m.base, &bag, collect_tag), 0);
    ASSERT_EQ_STR(tag_bag_get(&bag, "title"), "Auto MP3");

    tag_bag_free(&bag);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Tests: AudioStream abstraction                                     */
/* ------------------------------------------------------------------ */

static int test_audio_stream_read_null(void)
{
    uint8_t out[4];
    ASSERT_EQ_INT(audio_stream_read(NULL, 0, 4, out), -1);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    int passed = 0, failed = 0;

    /* NULL / invalid inputs */
    RUN_TEST(test_null_stream);
    RUN_TEST(test_null_callback);
    RUN_TEST(test_empty_stream);
    RUN_TEST(test_short_stream);
    RUN_TEST(test_audio_stream_read_null);

    /* FLAC */
    RUN_TEST(test_flac_basic_tags);
    RUN_TEST(test_flac_duration_calculation);
    RUN_TEST(test_flac_key_lowercase);
    RUN_TEST(test_flac_empty_value);
    RUN_TEST(test_flac_bad_magic);
    RUN_TEST(test_flac_missing_vorbis_comment);
    RUN_TEST(test_flac_truncated_streaminfo);
    RUN_TEST(test_flac_truncated_vorbis_comment);
    RUN_TEST(test_flac_zero_sample_rate);
    RUN_TEST(test_flac_zero_length_vorbis_comment);
    RUN_TEST(test_flac_vorbis_oversized_vendor);
    RUN_TEST(test_flac_vorbis_truncated_after_vendor);
    RUN_TEST(test_flac_vorbis_oversized_comment);
    RUN_TEST(test_flac_io_error_on_block_header);
    RUN_TEST(test_flac_io_error_on_streaminfo_read);

    /* MP3 / ID3v2 */
    RUN_TEST(test_mp3_basic_tags);
    RUN_TEST(test_mp3_date_frames);
    RUN_TEST(test_mp3_bad_magic);
    RUN_TEST(test_mp3_no_recognized_frames);
    RUN_TEST(test_mp3_duration_frame);
    RUN_TEST(test_mp3_encoding_latin1);
    RUN_TEST(test_mp3_encoding_utf16_bom);
    RUN_TEST(test_mp3_encoding_utf16be);
    RUN_TEST(test_mp3_encoding_latin1_to_utf8);
    RUN_TEST(test_mp3_id3v22);
    RUN_TEST(test_mp3_id3v24_syncsafe);
    RUN_TEST(test_mp3_extended_header);
    RUN_TEST(test_mp3_large_text_frame);
    RUN_TEST(test_mp3_unsupported_version);
    RUN_TEST(test_mp3_latin1_trailing_nuls);
    RUN_TEST(test_mp3_id3v24_extended_header);
    RUN_TEST(test_mp3_padding_after_frames);
    RUN_TEST(test_mp3_io_error_on_frame_data);
    RUN_TEST(test_mp3_bad_magic_long);
    RUN_TEST(test_mp3_utf8_trailing_nuls);
    RUN_TEST(test_mp3_io_error_on_ext_header);

    /* Auto-detect */
    RUN_TEST(test_autodetect_flac);
    RUN_TEST(test_autodetect_mp3);

    fprintf(stderr, "  %-50s%s\n", "---", "--");
    fprintf(stderr, "  %d passed, %d failed\n\n", passed, failed);

    return failed;
}
