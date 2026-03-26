#ifndef _FILE_AUDIO_STREAM_H_
#define _FILE_AUDIO_STREAM_H_

#include "audio_stream.h"

typedef struct FileAudioStream FileAudioStream;

// Open a AUDIO file and return a file-backed stream.
// Returns NULL on failure.
FileAudioStream* file_audio_stream_open(const char* filepath);

// Read data from the stream into the provided buffer.
int file_audio_stream_read(const AudioStream* stream, size_t offset, size_t length, uint8_t* out_buffer);

// Close a stream and free all associated resources.
void file_audio_stream_close(FileAudioStream* stream);

#endif /* _FILE_AUDIO_STREAM_H_ */
