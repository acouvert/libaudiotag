#ifndef _AUDIO_METADATA_H_
#define _AUDIO_METADATA_H_

#include "audio_stream.h"

#define AUDIO_EXPORT __attribute__((visibility("default")))

typedef void (*tag_found_cb)(void* ctx, const char* key, const char* value);

// Read metadata from the given AUDIO stream and invoke the callback for each tag found.
// Returns 0 on success, -1 on failure.
AUDIO_EXPORT int audio_stream_read_metadata(AudioStream* stream, void* callback_ctx, tag_found_cb callback);

#endif /* _AUDIO_METADATA_H_ */
