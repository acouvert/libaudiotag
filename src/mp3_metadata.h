#ifndef _MP3_METADATA_H_
#define _MP3_METADATA_H_

#include "audio_stream.h"
#include "audio_metadata.h"

// Read metadata from the given MP3 stream and invoke the callback for each tag found.
// Parses ID3v2 (v2.3/v2.4). Also extracts duration.
// Returns 0 on success, -1 on failure.
int mp3_read_metadata(AudioStream* stream, void* callback_ctx, tag_found_cb callback);

#endif /* _MP3_METADATA_H_ */
