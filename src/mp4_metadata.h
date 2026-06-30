#ifndef _MP4_METADATA_H_
#define _MP4_METADATA_H_

#include "audio_stream.h"
#include "audio_metadata.h"

// Read metadata from the given MP4/M4A stream and invoke the callback for each tag found.
// Parses the iTunes-style metadata atoms (moov/udta/meta/ilst). Also extracts duration
// from the movie header (moov/mvhd).
// Returns 0 on success, -1 on failure.
int mp4_read_metadata(AudioStream* stream, void* callback_ctx, tag_found_cb callback);

#endif /* _MP4_METADATA_H_ */
