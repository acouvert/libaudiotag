#ifndef _FLAC_METADATA_H_
#define _FLAC_METADATA_H_

#include "audio_stream.h"
#include "audio_metadata.h"

// Read metadata from the given FLAC stream and invoke the callback for each tag found.
// Returns 0 on success, -1 on failure.
int flac_read_metadata(AudioStream* stream, void* callback_ctx, tag_found_cb callback);

#endif /* _FLAC_METADATA_H_ */
