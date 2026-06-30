#include "mp4_metadata.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// -------- helpers (portable, independent of host endianness) --------

static inline uint16_t be16(const uint8_t b[2])
{
    return (uint16_t)(((uint16_t)b[0] << 8) | (uint16_t)b[1]);
}

static inline uint32_t be32(const uint8_t b[4])
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
}

static inline uint64_t be64(const uint8_t b[8])
{
    return ((uint64_t)b[0] << 56) | ((uint64_t)b[1] << 48) |
           ((uint64_t)b[2] << 40) | ((uint64_t)b[3] << 32) |
           ((uint64_t)b[4] << 24) | ((uint64_t)b[5] << 16) |
           ((uint64_t)b[6] <<  8) |  (uint64_t)b[7];
}

// -------- iTunes ilst atom name to lowercase tag name mapping --------
//
// The leading 0xA9 byte (the "©" copyright sign) is split out via string
// concatenation so the \x escape does not absorb the following letter.

typedef struct {
    const char atom[5];
    const char* tag_name;
} AtomMapping;

static const AtomMapping ilst_atoms[] = {
    { "\xA9" "nam", "title"       },
    { "\xA9" "ART", "artist"      },
    { "\xA9" "alb", "album"       },
    { "aART",       "albumartist" },
    { "\xA9" "day", "date"        },
    { "\xA9" "gen", "genre"       },
    { "\xA9" "wrt", "composer"    },
    { "trkn",       "tracknumber" },
    { "disk",       "discnumber"  },
    { "",           NULL          }  // sentinel
};

static const char* map_atom(const char atom[4])
{
    for (const AtomMapping* m = ilst_atoms; m->tag_name; m++)
    {
        if (memcmp(m->atom, atom, 4) == 0)
        {
            return m->tag_name;
        }
    }
    return NULL;
}

// -------- box (atom) traversal --------

// Read the box header at `offset`. `limit` is the exclusive end of the region the
// box lives in (SIZE_MAX when the upper bound is unknown, e.g. at the top level).
// On success fills the 4-byte type, the total box size (header + payload) and the
// header size (8, or 16 for 64-bit extended sizes).
static int read_box_header(
    AudioStream* stream,
    size_t       offset,
    size_t       limit,
    char         type_out[4],
    size_t*      box_size_out,
    size_t*      header_size_out)
{
    uint8_t hdr[8];
    if (audio_stream_read(stream, offset, 8, hdr) != 0)
    {
        return -1;
    }

    uint32_t size32 = be32(hdr);
    memcpy(type_out, &hdr[4], 4);

    size_t header_size = 8;
    size_t box_size;

    if (size32 == 1)
    {
        // 64-bit extended size follows the 8-byte header
        uint8_t ext[8];
        if (audio_stream_read(stream, offset + 8, 8, ext) != 0)
        {
            return -1;
        }
        header_size = 16;
        box_size = (size_t)be64(ext);
    }
    else if (size32 == 0)
    {
        // Box extends to the end of its enclosing region
        if (limit == SIZE_MAX)
        {
            return -1;
        }
        box_size = limit - offset;
    }
    else
    {
        box_size = size32;
    }

    if (box_size < header_size)
    {
        return -1;
    }

    *box_size_out    = box_size;
    *header_size_out = header_size;
    return 0;
}

// Find the first child box of the given type within [start, end). On success sets
// the payload range [content_off, content_end). Returns 0 if found, -1 otherwise.
static int find_box(
    AudioStream* stream,
    size_t       start,
    size_t       end,
    const char   type[4],
    size_t*      content_off,
    size_t*      content_end)
{
    size_t pos = start;

    for (;;)
    {
        if (end != SIZE_MAX && pos + 8 > end)
        {
            return -1;
        }

        char   t[4];
        size_t box_size;
        size_t header_size;
        if (read_box_header(stream, pos, end, t, &box_size, &header_size) != 0)
        {
            return -1;
        }

        if (memcmp(t, type, 4) == 0)
        {
            *content_off = pos + header_size;
            *content_end = pos + box_size;
            return 0;
        }

        size_t next = pos + box_size;
        if (next <= pos)
        {
            // No forward progress: malformed or overflow
            return -1;
        }
        pos = next;

        if (end != SIZE_MAX && pos > end)
        {
            return -1;
        }
    }
}

// -------- metadata value extraction --------

// Emit the value carried by a `data` atom whose payload spans [doff, dend).
// The payload layout is: u32 version+type, u32 locale, then the value bytes.
static void emit_data_atom(
    AudioStream* stream,
    const char*  tag_name,
    const char   atom[4],
    size_t       doff,
    size_t       dend,
    void*        ctx,
    tag_found_cb cb,
    bool*        found)
{
    if (dend < doff + 8)
    {
        return;
    }

    uint8_t hdr[8];
    if (audio_stream_read(stream, doff, 8, hdr) != 0)
    {
        return;
    }

    uint32_t value_type = be32(hdr) & 0x00FFFFFF; // low 24 bits = well-known type
    size_t   value_off  = doff + 8;
    size_t   value_len  = dend - value_off;

    // trkn / disk store a binary record: 2 reserved, u16 number, u16 total, ...
    if (memcmp(atom, "trkn", 4) == 0 || memcmp(atom, "disk", 4) == 0)
    {
        if (value_len < 4)
        {
            return;
        }

        uint8_t num[4];
        if (audio_stream_read(stream, value_off, 4, num) != 0)
        {
            return;
        }

        uint16_t number = be16(&num[2]);
        if (number == 0)
        {
            return;
        }

        char out[16];
        snprintf(out, sizeof(out), "%u", (unsigned)number);
        cb(ctx, tag_name, out);
        *found = true;
        return;
    }

    // Text value (well-known type 1 = UTF-8)
    if (value_type != 1 || value_len == 0)
    {
        return;
    }

    char  stack_buf[512];
    char* buf = (value_len < sizeof(stack_buf)) ? stack_buf : (char*)malloc(value_len + 1);
    if (!buf)
    {
        return;
    }

    if (audio_stream_read(stream, value_off, value_len, (uint8_t*)buf) != 0)
    {
        if (buf != stack_buf)
        {
            free(buf);
        }
        return;
    }

    buf[value_len] = '\0';
    cb(ctx, tag_name, buf);
    *found = true;

    if (buf != stack_buf)
    {
        free(buf);
    }
}

// Walk the ilst children, emitting a tag for each recognized metadata atom.
static void parse_ilst(
    AudioStream* stream,
    size_t       start,
    size_t       end,
    void*        ctx,
    tag_found_cb cb,
    bool*        found)
{
    size_t pos = start;

    while (pos + 8 <= end)
    {
        char   t[4];
        size_t box_size;
        size_t header_size;
        if (read_box_header(stream, pos, end, t, &box_size, &header_size) != 0)
        {
            break;
        }

        const char* tag_name = map_atom(t);
        if (tag_name)
        {
            size_t data_off;
            size_t data_end;
            if (find_box(stream, pos + header_size, pos + box_size, "data", &data_off, &data_end) == 0)
            {
                emit_data_atom(stream, tag_name, t, data_off, data_end, ctx, cb, found);
            }
        }

        size_t next = pos + box_size;
        if (next <= pos)
        {
            break;
        }
        pos = next;
    }
}

// -------- duration (moov/mvhd) --------

// Compute duration (ms) from the movie header. Emits a "duration" tag and returns
// true on success.
static bool emit_mp4_duration(
    AudioStream* stream,
    size_t       moov_off,
    size_t       moov_end,
    void*        ctx,
    tag_found_cb cb)
{
    size_t off;
    size_t end;
    if (find_box(stream, moov_off, moov_end, "mvhd", &off, &end) != 0)
    {
        return false;
    }

    uint8_t buf[32];
    size_t  avail = end - off;
    size_t  want  = (avail < sizeof(buf)) ? avail : sizeof(buf);
    if (want < 20 || audio_stream_read(stream, off, want, buf) != 0)
    {
        return false;
    }

    uint8_t  version = buf[0];
    uint32_t timescale;
    uint64_t duration;

    if (version == 1)
    {
        // version(1)+flags(3), creation(8), modification(8), timescale(4), duration(8)
        if (want < 32)
        {
            return false;
        }
        timescale = be32(&buf[20]);
        duration  = be64(&buf[24]);
    }
    else
    {
        // version(1)+flags(3), creation(4), modification(4), timescale(4), duration(4)
        timescale = be32(&buf[12]);
        duration  = be32(&buf[16]);
    }

    if (timescale == 0)
    {
        return false;
    }

    uint64_t duration_ms = (duration * 1000 + timescale / 2) / timescale;
    char out[32];
    snprintf(out, sizeof(out), "%llu", (unsigned long long)duration_ms);
    cb(ctx, "duration", out);
    return true;
}

// -------- public API --------

int mp4_read_metadata(
    AudioStream* stream,
    void*        callback_ctx,
    tag_found_cb callback)
{
    if (!stream || !callback)
    {
        return -1;
    }

    // The first box of an MP4/M4A file must be a file-type box.
    char   type[4];
    size_t box_size;
    size_t header_size;
    if (read_box_header(stream, 0, SIZE_MAX, type, &box_size, &header_size) != 0 ||
        memcmp(type, "ftyp", 4) != 0)
    {
        return -1;
    }

    // Locate the movie box.
    size_t moov_off;
    size_t moov_end;
    if (find_box(stream, 0, SIZE_MAX, "moov", &moov_off, &moov_end) != 0)
    {
        return -1;
    }

    bool found = false;

    if (emit_mp4_duration(stream, moov_off, moov_end, callback_ctx, callback))
    {
        found = true;
    }

    // Tags live at moov/udta/meta/ilst. `meta` is a FullBox: skip its 4-byte
    // version/flags field before descending into its children.
    size_t udta_off;
    size_t udta_end;
    if (find_box(stream, moov_off, moov_end, "udta", &udta_off, &udta_end) == 0)
    {
        size_t meta_off;
        size_t meta_end;
        if (find_box(stream, udta_off, udta_end, "meta", &meta_off, &meta_end) == 0 &&
            meta_off + 4 <= meta_end)
        {
            size_t ilst_off;
            size_t ilst_end;
            if (find_box(stream, meta_off + 4, meta_end, "ilst", &ilst_off, &ilst_end) == 0)
            {
                parse_ilst(stream, ilst_off, ilst_end, callback_ctx, callback, &found);
            }
        }
    }

    return found ? 0 : -1;
}
