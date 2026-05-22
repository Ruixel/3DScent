// Stolen from the 3DS examples 
//
/*
 * Fast, threaded Ogg Vorbis audio streaming example using libvorbisidec
 * (also known as libtremor) for libctru on Nintendo 3DS
 * 
 * Adapted to Vorbis by Théo B. (LiquidFenrir), originally written for Opus
 * by Lauren Kelly (thejsa) with help from mtheall.
 * See the opus-decoding example for more details
 * 
 * Last update: 2024-03-31
 */

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define START_CHANNEL 16

#include <tremor/ivorbisfile.h>
#include <tremor/ivorbiscodec.h>
#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "digi.h"

// ---- DEFINITIONS ----

static int THREAD_AFFINITY = -1;           // Execute thread on any core
static const int THREAD_STACK_SZ = 32 * 1024;    // 32kB stack for audio thread

// ---- END DEFINITIONS ----

ndspWaveBuf s_waveBufs[3];
int16_t *s_audioBuffer = NULL;

LightEvent s_event;
volatile bool s_quit = false;  // Quit flag
static LightLock s_vfLock;
static bool s_oggReady = false;

static OggVorbis_File s_vorbisFile;
static Thread s_threadId;
static bool s_oggLoaded = false;
static bool s_oggLoop = false;

typedef struct {
    char *data;
    size_t size;
    size_t pos;
} OggMemoryStream;

// ---- HELPER FUNCTIONS ----

// Retrieve strings for libvorbisidec errors
const char *vorbisStrError(int error)
{
    switch(error) {
        case OV_FALSE:
            return "OV_FALSE: A request did not succeed.";
        case OV_HOLE:
            return "OV_HOLE: There was a hole in the page sequence numbers.";
        case OV_EREAD:
            return "OV_EREAD: An underlying read, seek or tell operation "
                   "failed.";
        case OV_EFAULT:
            return "OV_EFAULT: A NULL pointer was passed where none was "
                   "expected, or an internal library error was encountered.";
        case OV_EIMPL:
            return "OV_EIMPL: The stream used a feature which is not "
                   "implemented.";
        case OV_EINVAL:
            return "OV_EINVAL: One or more parameters to a function were "
                   "invalid.";
        case OV_ENOTVORBIS:
            return "OV_ENOTVORBIS: This is not a valid Ogg Vorbis stream.";
        case OV_EBADHEADER:
            return "OV_EBADHEADER: A required header packet was not properly "
                   "formatted.";
        case OV_EVERSION:
            return "OV_EVERSION: The ID header contained an unrecognised "
                   "version number.";
        case OV_EBADPACKET:
            return "OV_EBADPACKET: An audio packet failed to decode properly.";
        case OV_EBADLINK:
            return "OV_EBADLINK: We failed to find data we had seen before or "
                   "the stream was sufficiently corrupt that seeking is "
                   "impossible.";
        case OV_ENOSEEK:
            return "OV_ENOSEEK: An operation that requires seeking was "
                   "requested on an unseekable stream.";
        default:
            return "Unknown error.";
    }
}

// ---- END HELPER FUNCTIONS ----

// Audio initialisation code
// This sets up NDSP and our primary audio buffer
bool audioInit(OggVorbis_File *vorbisFile_) {
    vorbis_info *vi = ov_info(vorbisFile_, -1);
    printf("rate=%ld channels=%d\n", vi->rate, vi->channels);

    // Setup NDSP
    ndspChnReset(START_CHANNEL);
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnSetInterp(START_CHANNEL, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(START_CHANNEL, vi->rate);
    ndspChnSetFormat(START_CHANNEL, vi->channels == 1
        ? NDSP_FORMAT_MONO_PCM16
        : NDSP_FORMAT_STEREO_PCM16);

    // Allocate audio buffer
    // 200ms buffer
    const size_t SAMPLES_PER_BUF = vi->rate * 200 / 1000;
    // mono (1) or stereo (2)
    const size_t CHANNELS_PER_SAMPLE = vi->channels;
    // s16 buffer
    const size_t WAVEBUF_SIZE = SAMPLES_PER_BUF * CHANNELS_PER_SAMPLE * sizeof(s16);
    const size_t bufferSize = WAVEBUF_SIZE * ARRAY_SIZE(s_waveBufs);
    s_audioBuffer = (int16_t *)linearAlloc(bufferSize);
    if(!s_audioBuffer) {
        printf("Failed to allocate audio buffer\n");
        return false;
    }

    // Setup waveBufs for NDSP
    memset(&s_waveBufs, 0, sizeof(s_waveBufs));
    int16_t *buffer = s_audioBuffer;

    for(size_t i = 0; i < ARRAY_SIZE(s_waveBufs); ++i) {
        s_waveBufs[i].data_vaddr = buffer;
        if (vi->channels == 1) {
          s_waveBufs[i].nsamples   = WAVEBUF_SIZE / sizeof(buffer[0]);
        } else {
          s_waveBufs[i].nsamples   = WAVEBUF_SIZE / (sizeof(buffer[0]) * 2);
        }
        s_waveBufs[i].status     = NDSP_WBUF_DONE;

        buffer += WAVEBUF_SIZE / sizeof(buffer[0]);
    }

    return true;
}

// Audio de-initialisation code
// Stops playback and frees the primary audio buffer
void audioExit(void) {
  ndspChnReset(START_CHANNEL);
  if (s_audioBuffer) {
    linearFree(s_audioBuffer);
    s_audioBuffer = NULL;
  }
}

// Main audio decoding logic
// This function pulls and decodes audio samples from vorbisFile_ to fill waveBuf_
bool fillBuffer(OggVorbis_File *vorbisFile_, ndspWaveBuf *waveBuf_) {
    vorbis_info *vi = ov_info(vorbisFile_, -1);
    const size_t bytes_per_frame = vi->channels * sizeof(s16);
    const size_t target_bytes = waveBuf_->nsamples * bytes_per_frame;

    int totalBytes = 0;
    while (totalBytes < target_bytes) {
        int16_t *buffer = waveBuf_->data_pcm16 + (totalBytes / sizeof(s16));
        const size_t bufferSize = target_bytes - totalBytes;

        const int bytesRead = ov_read(vorbisFile_, (char *)buffer, bufferSize, NULL);
        if (bytesRead < 0) {
            printf("ov_read: error %d (%s)\n", bytesRead, vorbisStrError(bytesRead));
            break;
        }
        if (bytesRead == 0) {
            // EOF
            if (s_oggLoop) {
                if (ov_pcm_seek(vorbisFile_, 0) != 0) {
                    printf("ov_pcm_seek failed, stopping\n");
                    break;  // treat as end-of-song
                }
                continue;  // keep filling
            } else {
                break;  // end of song, no loop
            }
        }
        totalBytes += bytesRead;
    }

    if (totalBytes == 0) {
        return false;  // buffer empty, don't queue it
    }

    waveBuf_->nsamples = totalBytes / bytes_per_frame;
    ndspChnWaveBufAdd(START_CHANNEL, waveBuf_);
    DSP_FlushDataCache(waveBuf_->data_pcm16, totalBytes);

    return true;
}

// NDSP audio frame callback
// This signals the audioThread to decode more things
// once NDSP has played a sound frame, meaning that there should be
// one or more available waveBufs to fill with more data.
void audioCallback(void *const nul_) {
    (void)nul_;  // Unused

    if(s_quit) { // Quit flag
        return;
    }
    
    LightEvent_Signal(&s_event);
}

// Audio thread
// This handles calling the decoder function to fill NDSP buffers as necessary
void audioThread(void *const arg) {
    (void)arg;  // we don't use it anymore — access s_vorbisFile globally

    while (!s_quit) {
        LightLock_Lock(&s_vfLock);
        
        if (!s_oggReady) {
            // No ogg loaded (or one is being swapped in) — just release and wait
            LightLock_Unlock(&s_vfLock);
            LightEvent_Wait(&s_event);
            continue;
        }

        for (size_t i = 0; i < ARRAY_SIZE(s_waveBufs); ++i) {
            if (s_waveBufs[i].status != NDSP_WBUF_DONE) continue;
            if (!fillBuffer(&s_vorbisFile, &s_waveBufs[i])) {
                printf("Playback complete\n");
                s_oggReady = false;
                break;
            }
        }
        
        LightLock_Unlock(&s_vfLock);
        LightEvent_Wait(&s_event);
    }
}

static size_t ogg_mem_read(void *ptr, size_t size, size_t nmemb, void *datasource) {
    OggMemoryStream *s = (OggMemoryStream *)datasource;
    size_t want = size * nmemb;
    size_t avail = s->size - s->pos;
    size_t n = want < avail ? want : avail;
    if (n > 0) {
        memcpy(ptr, s->data + s->pos, n);
        s->pos += n;
    }
    return n / size;
}

static int ogg_mem_seek(void *datasource, ogg_int64_t offset, int whence) {
    OggMemoryStream *s = (OggMemoryStream *)datasource;
    size_t np;
    switch (whence) {
        case SEEK_SET: np = (size_t)offset; break;
        case SEEK_CUR: np = s->pos + (size_t)offset; break;
        case SEEK_END: np = s->size + (size_t)offset; break;
        default: return -1;
    }
    if (np > s->size) return -1;
    s->pos = np;
    return 0;
}

static long ogg_mem_tell(void *datasource) {
    return (long)((OggMemoryStream *)datasource)->pos;
}

static int ogg_mem_close(void *datasource) {
    OggMemoryStream *s = (OggMemoryStream *)datasource;
    free(s->data);
    free(s);
    return 0;
}

bool load_ogg_from_music_library(struct MusicLibrary *library, char *fileName, bool loop) {
    printf("There are %d files in the archive\n", mz_zip_reader_get_num_files(&library->zip));
    int file_index = mz_zip_reader_locate_file(&library->zip, fileName, NULL, 0);
    if (file_index < 0) {
        printf("File '%s' not in archive\n", fileName);
        return false;
    }

    mz_zip_archive_file_stat file_stat;
    mz_zip_reader_file_stat(&library->zip, file_index, &file_stat);

    OggMemoryStream *stream = malloc(sizeof(OggMemoryStream));
    stream->size = (size_t)file_stat.m_uncomp_size;
    stream->pos = 0;
    stream->data = malloc(stream->size);

    if (!mz_zip_reader_extract_to_mem(&library->zip, file_index, stream->data, stream->size, 0)) {
        printf("extract failed for '%s'\n", fileName);
        free(stream->data);
        free(stream);
        return false;
    }

    printf("Loaded '%s' from archive, size %zu bytes\n", fileName, stream->size);

    LightLock_Lock(&s_vfLock);
    
    if (s_oggReady) {
        s_oggReady = false;
        ov_clear(&s_vorbisFile);
        audioExit();
    }
    
    ov_callbacks cb = { .read_func = ogg_mem_read, .close_func = ogg_mem_close,
                         .seek_func = ogg_mem_seek, .tell_func = ogg_mem_tell };
    int error = ov_open_callbacks(stream, &s_vorbisFile, NULL, 0, cb);
    if (error) {
        LightLock_Unlock(&s_vfLock);
        printf("ov_open_callbacks: %d (%s)\n", error, vorbisStrError(error));
        free(stream->data);
        free(stream);
        return false;
    }
    
    if (!audioInit(&s_vorbisFile)) {
        ov_clear(&s_vorbisFile);
        LightLock_Unlock(&s_vfLock);
        return false;
    }
    
    s_oggReady = true;
    s_oggLoop = loop;
    LightLock_Unlock(&s_vfLock);
    
    if (s_threadId == 0) {
        int32_t priority = 0x30;
        svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
        priority -= 1;
        if (priority < 0x18) priority = 0x18;
        if (priority > 0x3F) priority = 0x3F;

        s_threadId = threadCreate(audioThread, NULL,
                                  THREAD_STACK_SZ, priority,
                                  THREAD_AFFINITY, false);
    }
    
    LightEvent_Signal(&s_event);
    
    return true;
}


void initOggPlayer(void) {
    // Check if old 3ds 
    bool isNew3ds;
    APT_CheckNew3DS(&isNew3ds);

    if (!isNew3ds) {
      APT_SetAppCpuTimeLimit(30); // Core 1 
      THREAD_AFFINITY = 1;
    } 

    // Setup LightEvent for synchronisation of audioThread
    LightEvent_Init(&s_event, RESET_ONESHOT);
    LightLock_Init(&s_vfLock);

    // Set the ndsp sound frame callback which signals our audioThread
    ndspSetCallback(audioCallback, NULL);
}

void shutdownOggPlayer(void) {
    // Stop ndsp from invoking our callback during teardown
    ndspSetCallback(NULL, NULL);
    
    // Signal audio thread to quit
    s_quit = true;
    LightEvent_Signal(&s_event);

    // Wait for the audio thread to finish
    if (s_threadId) {
        threadJoin(s_threadId, UINT64_MAX);
        threadFree(s_threadId);
        s_threadId = 0;
    }

    LightLock_Lock(&s_vfLock);
    if (s_oggReady) {
        s_oggReady = false;
        ov_clear(&s_vorbisFile);
    }
    audioExit();
    LightLock_Unlock(&s_vfLock);
}
