#ifndef _AUDIO_STREAM_H_
#define _AUDIO_STREAM_H_

#include <stddef.h>
#include <stdint.h>

typedef struct AudioStream AudioStream;

typedef int  (*audio_stream_read_fn) (const AudioStream* stream, size_t offset, size_t length, uint8_t* out_buffer);

struct AudioStream {
    audio_stream_read_fn read;
};

int  audio_stream_read(const AudioStream* stream, size_t offset, size_t length, uint8_t* out_buffer);

#endif /* _AUDIO_STREAM_H_ */
