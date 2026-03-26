#include "file_audio_stream.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

struct FileAudioStream{
    AudioStream base; // must be first member
    uint8_t* file_data;
    size_t file_size;
};

int file_audio_stream_read(const AudioStream* stream, size_t offset, size_t length, uint8_t* out_buffer)
{
    const FileAudioStream* f = (const FileAudioStream*)stream;
    if (!f->file_data || offset > f->file_size || length > f->file_size - offset)
    {
        return -1;
    }

    memcpy(out_buffer, f->file_data + offset, length);
    return 0;
}

// Open an audio file and return a file-backed stream.
// Returns NULL on failure.
FileAudioStream* file_audio_stream_open(const char* filepath)
{
    int fd = open(filepath, O_RDONLY);
    if (fd < 0)
    {
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) == 0)
    {
        FileAudioStream* f = (FileAudioStream*)calloc(1, sizeof(FileAudioStream));
        if (f)
        {
            f->file_size = (size_t)st.st_size;
            f->file_data = (uint8_t*)mmap(NULL, f->file_size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (f->file_data != MAP_FAILED)
            {
                f->base.read  = file_audio_stream_read;
                close(fd);
                return f;
            }

            free(f);
        }
    }

    close(fd);
    return NULL;
}

// Close a stream and free all associated resources.
void file_audio_stream_close(FileAudioStream* stream)
{
    if (!stream)
    {
        return;
    }

    if (stream->file_data)
    {
        munmap(stream->file_data, stream->file_size);
    }

    free(stream);
}
