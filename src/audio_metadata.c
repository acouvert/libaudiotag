#include "audio_metadata.h"
#include "flac_metadata.h"
#include "mp3_metadata.h"

int audio_stream_read_metadata(AudioStream* stream, void* callback_ctx, tag_found_cb callback)
{
    // Try FLAC first, fall back to MP3
    if (flac_read_metadata(stream, callback_ctx, callback) == 0)
    {
        return 0;
    }

    return mp3_read_metadata(stream, callback_ctx, callback);
}
