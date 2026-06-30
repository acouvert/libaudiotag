#include "mp3_metadata.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// -------- helpers --------

static inline uint32_t be24(const uint8_t b[3])
{
    return ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | (uint32_t)b[2];
}

static inline uint32_t be32(const uint8_t b[4])
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
}

// ID3v2 sizes use syncsafe encoding (7 bits per byte)
static inline uint32_t syncsafe32(const uint8_t b[4])
{
    return ((uint32_t)(b[0] & 0x7F) << 21) |
           ((uint32_t)(b[1] & 0x7F) << 14) |
           ((uint32_t)(b[2] & 0x7F) <<  7) |
            (uint32_t)(b[3] & 0x7F);
}

// -------- ID3v2 frame-id to lowercase tag name mapping --------

typedef struct {
    const char frame_id[5];
    const char* tag_name;
} FrameMapping;

static const FrameMapping id3v2_frames[] = {
    { "TIT2", "title"       },
    { "TPE1", "artist"      },
    { "TALB", "album"       },
    { "TRCK", "tracknumber" },
    { "TYER", "date"        },
    { "TDRC", "date"        },
    { "TCON", "genre"       },
    { "TPE2", "albumartist" },
    { "TLEN", "duration"   },
    { "",     NULL          }  // sentinel
};

static const char* map_frame_id(const char id[4])
{
    for (const FrameMapping* m = id3v2_frames; m->frame_id[0]; m++)
    {
        if (memcmp(m->frame_id, id, 4) == 0)
        {
            return m->tag_name;
        }
    }
    return NULL;
}

// -------- ID3v2 parsing --------

typedef struct {
    uint8_t  version_major; // 3 or 4
    uint8_t  version_minor;
    uint8_t  flags;
    uint32_t size;          // tag body size (excludes 10-byte header)
} Id3v2Header;

static int read_id3v2_header(AudioStream* stream, Id3v2Header* out)
{
    uint8_t buf[10];
    if (audio_stream_read(stream, 0, 10, buf) != 0)
    {
        return -1;
    }

    if (buf[0] != 'I' || buf[1] != 'D' || buf[2] != '3')
    {
        return -1;
    }

    out->version_major = buf[3];
    out->version_minor = buf[4];
    out->flags         = buf[5];
    out->size          = syncsafe32(&buf[6]);

    if (out->version_major < 2 || out->version_major > 4)
    {
        return -1;
    }

    return 0;
}

// Emit a text-frame value. Handles the encoding byte and strips BOM/nul terminator.
static void emit_text_frame(
    const char*  tag_name,
    const uint8_t* data,
    uint32_t       data_len,
    void*          ctx,
    tag_found_cb   cb)
{
    if (data_len < 1)
    {
        return;
    }

    uint8_t encoding = data[0];
    const uint8_t* text = data + 1;
    uint32_t text_len = data_len - 1;

    char stack_buf[512];
    char* buf = NULL;
    size_t out_len = 0;

    // UTF-16 with BOM (1) and UTF-16BE (2): skip BOM and do a lossy ASCII extraction.
    if (encoding == 1 || encoding == 2)
    {
        if (text_len >= 2 && ((text[0] == 0xFF && text[1] == 0xFE) ||
                              (text[0] == 0xFE && text[1] == 0xFF)))
        {
            text     += 2;
            text_len -= 2;
        }

        buf = (text_len < sizeof(stack_buf)) ? stack_buf : (char*)malloc(text_len + 1);
        if (!buf)
        {
            return;
        }

        for (uint32_t i = 0; i < text_len; i++)
        {
            if (text[i] != 0)
            {
                buf[out_len++] = (char)text[i];
            }
        }
    }
    else
    {
        if (encoding == 0)
        {
            // Latin-1 → UTF-8: each byte 0x80–0xFF becomes 2 UTF-8 bytes
            size_t max_out = (size_t)text_len * 2 + 1;
            buf = (max_out <= sizeof(stack_buf)) ? stack_buf : (char*)malloc(max_out);
            if (!buf)
            {
                return;
            }

            for (uint32_t i = 0; i < text_len; i++)
            {
                uint8_t ch = text[i];
                if (ch == 0)
                {
                    continue;
                }
                if (ch < 0x80)
                {
                    buf[out_len++] = (char)ch;
                }
                else
                {
                    buf[out_len++] = (char)(0xC0 | (ch >> 6));
                    buf[out_len++] = (char)(0x80 | (ch & 0x3F));
                }
            }
        }
        else
        {
            // encoding 3 (UTF-8): pass through
            buf = (text_len < sizeof(stack_buf)) ? stack_buf : (char*)malloc(text_len + 1);
            if (!buf)
            {
                return;
            }

            memcpy(buf, text, text_len);
            out_len = text_len;

            // Trim trailing NUL chars
            while (out_len > 0 && buf[out_len - 1] == '\0')
            {
                out_len--;
            }
        }
    }

    buf[out_len] = '\0';
    if (out_len > 0)
    {
        cb(ctx, tag_name, buf);
    }
    if (buf != stack_buf)
    {
        free(buf);
    }
}

static int parse_id3v2_frames(
    AudioStream* stream,
    Id3v2Header* header,
    void*        ctx,
    tag_found_cb cb)
{
    uint32_t tag_size = header->size;
    size_t   base     = 10; // offset past the 10-byte ID3v2 header
    uint32_t pos      = 0;

    // Skip extended header if present
    if (header->flags & 0x40)
    {
        uint8_t ext[4];
        if (audio_stream_read(stream, base, 4, ext) != 0)
        {
            return -1;
        }

        uint32_t ext_size = (header->version_major == 4)
                          ? syncsafe32(ext)
                          : be32(ext);
        pos = ext_size;
    }

    bool found_any = false;
    int frame_hdr_size = (header->version_major >= 3) ? 10 : 6;
    uint8_t hdr_buf[10]; // large enough for v2.3/v2.4 (10) and v2.2 (6)

    while (pos + (uint32_t)frame_hdr_size <= tag_size)
    {
        if (audio_stream_read(stream, base + pos, (size_t)frame_hdr_size, hdr_buf) != 0)
        {
            break;
        }

        // Padding detection: NUL byte in frame-id position
        if (hdr_buf[0] == 0)
        {
            break;
        }

        char frame_id[5] = {0};
        uint32_t frame_size;

        if (header->version_major >= 3)
        {
            memcpy(frame_id, hdr_buf, 4);
            frame_size = (header->version_major == 4)
                       ? syncsafe32(&hdr_buf[4])
                       : be32(&hdr_buf[4]);
        }
        else
        {
            // ID3v2.2: 3-byte frame IDs, 3-byte sizes
            memcpy(frame_id, hdr_buf, 3);
            frame_id[3] = '\0';
            frame_size = be24(&hdr_buf[3]);
        }

        pos += (uint32_t)frame_hdr_size;

        if (frame_size == 0 || pos + frame_size > tag_size)
        {
            break;
        }

        // Only read frame data for frames we care about
        const char* tag_name = map_frame_id(frame_id);
        if (tag_name)
        {
            uint8_t stack_frame[512];
            uint8_t* frame_data = (frame_size <= sizeof(stack_frame))
                                ? stack_frame
                                : (uint8_t*)malloc(frame_size);
            if (!frame_data)
            {
                break;
            }

            if (audio_stream_read(stream, base + pos, frame_size, frame_data) != 0)
            {
                if (frame_data != stack_frame)
                {
                    free(frame_data);
                }
                break;
            }

            emit_text_frame(tag_name, frame_data, frame_size, ctx, cb);
            found_any = true;

            if (frame_data != stack_frame)
            {
                free(frame_data);
            }
        }

        pos += frame_size;
    }

    return found_any ? 0 : -1;
}

// -------- MPEG audio frame / Xing-Info duration --------

// Layer III bitrate tables (kbps) indexed by the 4-bit bitrate field.
static const int br_v1_l3[16] = { 0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0 };
static const int br_v2_l3[16] = { 0, 8,16,24,32,40,48,56,64, 80, 96,112,128,144,160,0 };
static const int sample_rates[3][4] = {
    { 44100, 48000, 32000, 0 }, // MPEG1
    { 22050, 24000, 16000, 0 }, // MPEG2
    { 11025, 12000,  8000, 0 }, // MPEG2.5
};

// Compute duration (ms) from the Xing/Info VBR header of the first MPEG frame,
// or fall back to a CBR estimate. Emits a "duration" tag; returns true on success.
static bool emit_mp3_duration(
    AudioStream* stream,
    size_t       first_frame,
    void*        ctx,
    tag_found_cb cb)
{
    uint8_t hdr[4];
    if (audio_stream_read(stream, first_frame, 4, hdr) != 0)
    {
        return false;
    }

    if (hdr[0] != 0xFF || (hdr[1] & 0xE0) != 0xE0)
    {
        return false;
    }

    int version_bits = (hdr[1] >> 3) & 0x3; // 0=2.5, 2=2, 3=1
    int layer_bits    = (hdr[1] >> 1) & 0x3; // 1=III
    int bitrate_idx   = (hdr[2] >> 4) & 0xF;
    int sr_idx        = (hdr[2] >> 2) & 0x3;
    int mono          = ((hdr[3] >> 6) & 0x3) == 3;

    if (version_bits == 1 || layer_bits != 1 || sr_idx == 3 ||
        bitrate_idx == 0 || bitrate_idx == 15)
    {
        return false;
    }

    int is_mpeg1 = (version_bits == 3);
    int sr_table = is_mpeg1 ? 0 : (version_bits == 2 ? 1 : 2);
    int sample_rate = sample_rates[sr_table][sr_idx];
    int samples_per_frame = is_mpeg1 ? 1152 : 576;

    // Xing/Info tag sits at a fixed offset after the frame header.
    size_t xing_off = first_frame + 4 + (is_mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17));
    uint8_t xing[12];
    if (audio_stream_read(stream, xing_off, 12, xing) == 0 &&
        (memcmp(xing, "Xing", 4) == 0 || memcmp(xing, "Info", 4) == 0))
    {
        uint32_t flags = be32(&xing[4]);
        if (flags & 0x1) // frame-count present
        {
            uint32_t frames = be32(&xing[8]);
            uint64_t duration_ms = (uint64_t)frames * samples_per_frame * 1000 / sample_rate;
            char out[32];
            snprintf(out, sizeof(out), "%llu", (unsigned long long)duration_ms);
            cb(ctx, "duration", out);
            return true;
        }
    }

    return false;
}

// -------- public API --------

int mp3_read_metadata(
    AudioStream* stream,
    void*        callback_ctx,
    tag_found_cb callback)
{
    if (!stream || !callback)
    {
        return -1;
    }

    Id3v2Header id3v2;
    if (read_id3v2_header(stream, &id3v2) != 0)
    {
        return -1;
    }

    int frames_ok = parse_id3v2_frames(stream, &id3v2, callback_ctx, callback);

    // First MPEG audio frame begins right after the ID3v2 tag (and footer, if any).
    size_t first_frame = 10 + id3v2.size + ((id3v2.flags & 0x10) ? 10 : 0);
    bool duration_ok = emit_mp3_duration(stream, first_frame, callback_ctx, callback);

    return (frames_ok == 0 || duration_ok) ? 0 : -1;
}
