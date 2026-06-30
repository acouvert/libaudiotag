#include "audio_metadata.h"
#include "flac_metadata.h"
#include "mp3_metadata.h"
#include "mp4_metadata.h"

int audio_stream_read_metadata(AudioStream* stream, void* callback_ctx, tag_found_cb callback)
{
    // Try FLAC first, then MP3, fall back to MP4/M4A
    if (flac_read_metadata(stream, callback_ctx, callback) == 0)
    {
        return 0;
    }

    if (mp3_read_metadata(stream, callback_ctx, callback) == 0)
    {
        return 0;
    }

    return mp4_read_metadata(stream, callback_ctx, callback);
}
