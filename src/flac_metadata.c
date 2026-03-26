#include "flac_metadata.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct
{
    uint8_t  type;
    uint8_t  is_last;
    uint32_t length;
} FlacMetadataBlockHeader;

typedef struct
{
    // 0..1: u(16) min block size (samples)
    uint8_t min_block_size_be[2];

    // 2..3: u(16) max block size (samples)
    uint8_t max_block_size_be[2];

    // 4..6: u(24) min frame size (bytes) (0 = unknown)
    uint8_t min_frame_size_be[3];

    // 7..9: u(24) max frame size (bytes) (0 = unknown)
    uint8_t max_frame_size_be[3];

    // 10..17: 64-bit composite:
    //   u(20) sample_rate
    //   u(3)  (channels-1)
    //   u(5)  (bits_per_sample-1)
    //   u(36) total_samples
    uint8_t sr_ch_bps_total_be[8];

    // 18..33: u(128) MD5 of unencoded audio
    uint8_t md5[16];
} AudioStreamInfoRaw;

// -------- helpers (portable, independent of host endianness) --------

static inline uint32_t be24(const uint8_t b[3]) {
    return ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | (uint32_t)b[2];
}

static inline uint32_t le32(const uint8_t b[4])
{
    return b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static inline uint64_t be64(const uint8_t b[8])
{
    return ((uint64_t)b[0] << 56) | ((uint64_t)b[1] << 48) |
           ((uint64_t)b[2] << 40) | ((uint64_t)b[3] << 32) |
           ((uint64_t)b[4] << 24) | ((uint64_t)b[5] << 16) |
           ((uint64_t)b[6] <<  8) |  (uint64_t)b[7];
}

// -------- decoded view accessors --------

static inline uint32_t flac_sample_rate_hz(const AudioStreamInfoRaw* s)
{
    uint64_t v = be64(s->sr_ch_bps_total_be);
    return (uint32_t)((v >> 44) & 0xFFFFF); // top 20 bits
}

static inline uint64_t flac_total_samples(const AudioStreamInfoRaw* s)
{
    uint64_t v = be64(s->sr_ch_bps_total_be);
    return (uint64_t)(v & 0xFFFFFFFFFULL); // low 36 bits
}

static int read_metadata_block_header(
    AudioStream* stream,
    size_t offset,
    FlacMetadataBlockHeader* out)
{
    uint8_t data[4];
    if (audio_stream_read(stream, offset, sizeof(data), data) != 0)
    {
        return -1;
    }

    out->is_last = (data[0] >> 7) & 1;
    out->type    = data[0] & 0x7F;
    out->length  = be24(&data[1]);

    return 0;
}

static int read_stream_info_block(
    AudioStream* stream,
    size_t offset,
    size_t length,
    void* callback_ctx,
    tag_found_cb callback)
{
    if (length < sizeof(AudioStreamInfoRaw))
    {
        return -1;
    }

    AudioStreamInfoRaw raw;
    if (audio_stream_read(stream, offset, sizeof(AudioStreamInfoRaw), (uint8_t *)&raw) != 0)
    {
        return -1;
    }

    uint32_t sample_rate = flac_sample_rate_hz(&raw);
    if (sample_rate > 0)
    {
        uint64_t duration_ms = (flac_total_samples(&raw) * 1000 + sample_rate / 2) / sample_rate;
        char duration_ms_str[32];
        snprintf(duration_ms_str, sizeof(duration_ms_str), "%" PRIu64, duration_ms);
        callback(callback_ctx, "duration", duration_ms_str);
    }

    return 0;
}

static int skip(
    uint8_t** block,
    size_t* block_length,
    size_t length)
{
    if (*block_length < length)
    {
        return -1;
    }

    *block += length;
    *block_length -= length;
    return 0;
}

static int read_le32(
    uint8_t** block,
    size_t* block_length,
    uint32_t* out)
{
    if (*block_length < sizeof(uint32_t))
    {
        return -1;
    }

    *out = le32(*block);
    *block += sizeof(uint32_t);
    *block_length -= sizeof(uint32_t);
    return 0;
}

static int parse_vorbis_comment_block(
    uint8_t* block,
    size_t length,
    void* callback_ctx,
    tag_found_cb callback)
{
    // Skip vendor string
    int ret = -1;
    uint32_t vendor_length;
    if ((ret = read_le32(&block, &length, &vendor_length)) != 0)
    {
        return ret;
    }

    if ((ret = skip(&block, &length, vendor_length)) != 0)
    {
        return ret;
    }

    // Comment count
    uint32_t comment_count;
    if ((ret = read_le32(&block, &length, &comment_count)) != 0)
    {
        return ret;
    }

    // Iterate over each comment and invoke the callback
    for (uint32_t i = 0; i < comment_count; i++)
    {
        uint32_t comment_length;
        if ((ret = read_le32(&block, &length, &comment_length)) != 0)
        {
            return ret;
        }

        char* comment = (char*)block;
        if ((ret = skip(&block, &length, comment_length)) != 0)
        {
            return ret;
        }

        const char* eq = memchr(comment, '=', comment_length);
        if (eq)
        {
            size_t key_len = (size_t)(eq - comment);
            size_t val_len = comment_length - key_len - 1;

            // NUL-terminate key in-place
            char* key = comment;
            key[key_len] = '\0';

            // Lowercase key in-place (block is our own mutable copy)
            for (size_t j = 0; j < key_len; j++)
            {
                key[j] = (char)tolower((unsigned char)key[j]);
            }

            // NUL-terminate value in-place
            char* val = key + key_len + 1;
            char saved_end = val[val_len];
            val[val_len] = '\0';

            callback(callback_ctx, key, val);

            // Restore last character which is also the first of the next comment (or end of block)
            val[val_len] = saved_end;
        }
    }

    return 0;
}

static int read_vorbis_comment_block(
    AudioStream* stream,
    size_t offset,
    size_t length,
    void* callback_ctx,
    tag_found_cb callback)
{
    if (length == 0)
    {
        return -1;
    }

    uint8_t* block = (uint8_t*)malloc(length);
    if (!block)
    {
        return -1;
    }

    int ret = -1;
    if (audio_stream_read(stream, offset, length, block) == 0)
    {
        ret = parse_vorbis_comment_block(block, length, callback_ctx, callback);
    }

    free(block);
    return ret;
}

int flac_read_metadata(
    AudioStream* stream,
    void* callback_ctx,
    tag_found_cb callback)
{
    if (!stream || !callback)
    {
        return -1;
    }

    FlacMetadataBlockHeader header;
    size_t offset = 0;
    bool stream_info_block_found = false;
    bool vorbis_comment_block_found = false;

    // Verify fLaC marker
    uint8_t marker[4];
    if (audio_stream_read(stream, 0, sizeof(marker), marker) != 0 ||
        memcmp(marker, "fLaC", 4) != 0)
    {
        return -1;
    }

    // Metadata blocks start immediately after the 4-byte marker
    offset += 4;

    while (!stream_info_block_found || !vorbis_comment_block_found)
    {
        if (read_metadata_block_header(stream, offset, &header) != 0)
        {
            // Corrupt or truncated metadata block header
            break;
        }

        // Header is 4 bytes, followed by `header.length` bytes of block data
        offset += 4;

        if (header.type == 0)
        {
            // STREAMINFO
            if (read_stream_info_block(stream, offset, header.length, callback_ctx, callback) != 0)
            {
                // STREAMINFO block too short or unreadable
                break;
            }

            stream_info_block_found = true;
        }
        else if (header.type == 4)
        {
            // VORBIS_COMMENT
            if (read_vorbis_comment_block(stream, offset, header.length, callback_ctx, callback) != 0)
            {
                // VORBIS_COMMENT block corrupt or unreadable
                break;
            }

            vorbis_comment_block_found = true;
        }

        offset += header.length;
        if (header.is_last)
        {
            break;
        }
    }

    return (stream_info_block_found || vorbis_comment_block_found) ? 0 : -1;
}
