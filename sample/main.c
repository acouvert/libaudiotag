#include <stdio.h>
#include "audio_metadata.h"
#include "file_audio_stream.h"

static void on_tag(void* ctx, const char* key, const char* value)
{
    (void)ctx;
    printf("  %s = %s\n", key, value);
}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <audio-file>\n", argv[0]);
        return 1;
    }

    FileAudioStream* stream = file_audio_stream_open(argv[1]);
    if (!stream)
    {
        fprintf(stderr, "Failed to open: %s\n", argv[1]);
        return 1;
    }

    printf("Tags for %s:\n", argv[1]);
    int ret = audio_stream_read_metadata((AudioStream*)stream, NULL, on_tag);
    if (ret != 0)
    {
        fprintf(stderr, "No metadata found.\n");
    }

    file_audio_stream_close(stream);
    return ret;
}
