# Audio Metadata Library

A C library for reading metadata tags from audio files. Supports FLAC and MP3 (ID3v2) formats.

## Building

Requires a C99 compiler, POSIX environment (Linux, macOS, WSL), and `gcov` for coverage.

```sh
make            # builds static lib, shared lib, and sample programs
make lib        # packages libraries and headers into bin/ for distribution
make test       # builds and runs unit tests
make coverage   # builds with gcov, runs tests, prints per-file coverage summary
make clean      # removes all build artifacts
```

Output (in `build/`):
- `libaudiotag.a` — static library
- `libaudiotag.so` — shared library (built with `-fPIC -fvisibility=hidden`)
- `sample/main` — example program that prints all tags from an audio file

`make lib` creates a `bin/` directory with everything needed to use the library:

```
bin/
  libaudiotag.a
  libaudiotag.so
  include/
    audio_metadata.h
    audio_stream.h
```

Only the public API headers are exported. Format-specific headers (`flac_metadata.h`, `mp3_metadata.h`) and the reference `FileAudioStream` implementation are not included — use `audio_stream_read_metadata()` as the single entry point and provide your own `AudioStream` backend.

To link against the packaged library:

```sh
cc -o myapp myapp.c -Ilibaudiotag/bin/include -Llibaudiotag/bin -laudiotag
```

## Public API

### AudioStream (audio_stream.h)

An abstract byte-stream interface used by all parsers.

```c
typedef int (*audio_stream_read_fn)(const AudioStream* stream, size_t offset, size_t length, uint8_t* out_buffer);

struct AudioStream {
    audio_stream_read_fn read;
};

int audio_stream_read(const AudioStream* stream, size_t offset, size_t length, uint8_t* out_buffer);
```

- `audio_stream_read` — Reads `length` bytes starting at `offset` into `out_buffer`. Returns 0 on success, -1 on failure. Delegates to the `read` function pointer stored in the struct.

Custom stream backends can be implemented by embedding `AudioStream` as the first member of a struct and assigning the `read` function pointer. A reference `mmap`-backed implementation (`FileAudioStream`) is provided in `sample/`.

### audio_stream_read_metadata (audio_metadata.h)

Top-level entry point that auto-detects the format.

```c
typedef void (*tag_found_cb)(void* ctx, const char* key, const char* value);

int audio_stream_read_metadata(AudioStream* stream, void* callback_ctx, tag_found_cb callback);
```

Tries FLAC first; if that fails, falls back to MP3. For each metadata tag found, `callback` is invoked with a lowercase key and a NUL-terminated value string. Returns 0 on success, -1 if no parser succeeded.

The `AUDIO_EXPORT` attribute is applied to this symbol for shared-library visibility.

### flac_read_metadata (flac_metadata.h)

```c
int flac_read_metadata(AudioStream* stream, void* callback_ctx, tag_found_cb callback);
```

Parses FLAC metadata blocks. Extracts:
- **STREAMINFO** (block type 0) — computes `duration` in milliseconds from `total_samples` and `sample_rate`.
- **VORBIS_COMMENT** (block type 4) — emits each `KEY=value` comment as a callback with the key lowercased.

Returns 0 if at least one of STREAMINFO or VORBIS_COMMENT was found. Missing blocks are silently skipped.

### mp3_read_metadata (mp3_metadata.h)

```c
int mp3_read_metadata(AudioStream* stream, void* callback_ctx, tag_found_cb callback);
```

Parses ID3v2 tags (versions 2.2, 2.3, and 2.4). Mapped text frames:

| Frame ID | Tag name      |
|----------|---------------|
| TIT2     | title         |
| TPE1     | artist        |
| TALB     | album         |
| TRCK     | tracknumber   |
| TYER     | date          |
| TDRC     | date          |
| TCON     | genre         |
| TPE2     | albumartist   |
| TLEN     | duration      |

Handles text encodings: Latin-1 (0), UTF-16 with BOM (1), UTF-16BE (2), and UTF-8 (3). Latin-1 is losslessly converted to UTF-8. UTF-16 is decoded with a lossy ASCII extraction (non-NUL bytes are kept as-is).

Returns 0 if at least one recognized frame was found and parsed, -1 otherwise.

## Behavior

1. All functions return 0 on success and -1 on failure.
2. `audio_stream_read_metadata` is the recommended entry point — it probes the stream for a `fLaC` marker, then falls back to checking for an `ID3` header.
3. Tags are delivered one at a time via the callback. Keys are always lowercase. Values are NUL-terminated C strings.
4. The caller owns the `AudioStream` lifetime; none of the metadata functions open or close streams.

## Known Limitations

### Metadata

- **No ID3v1 support.** Only ID3v2 tags are parsed for MP3 files.
- **Lossy UTF-16 handling.** UTF-16 text frames in ID3v2 are converted by dropping NUL bytes, which corrupts non-ASCII characters. Latin-1 frames are losslessly converted to UTF-8.
- **No FLAC PICTURE or PADDING blocks.** Only block types 0 (STREAMINFO) and 4 (VORBIS_COMMENT) are read; all others are skipped.
- **MP3 duration from TLEN only.** Duration is taken from the ID3v2 `TLEN` text frame if present; there is no fallback to computing duration from audio frame headers or Xing/VBRI headers.
- **No seeking or frame-level audio parsing.** The library only reads metadata; it does not decode audio frames.

### Library

- **Vorbis comment parsing mutates the buffer.** Keys and values are NUL-terminated in-place in a `malloc`-ed copy of the block. The original stream data is unaffected.
- **No thread safety guarantees.** Concurrent reads on the same `AudioStream` are not safe unless the underlying `read` implementation is thread-safe.

### Environment

- **Linux/POSIX only.** The sample `FileAudioStream` in `sample/` depends on `mmap`, `open`, and `fstat`. The core library itself has no platform-specific dependencies beyond C99 and POSIX `string.h`.
