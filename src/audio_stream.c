#include "audio_stream.h"

int audio_stream_read(const AudioStream* s, size_t offset, size_t length, uint8_t* out)
{
    if (!s || !s->read)
    {
        return -1;
    }

    return s->read(s, offset, length, out);
}
