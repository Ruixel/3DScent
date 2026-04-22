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
#include <string.h>

#include "digi.h"

// ---- DEFINITIONS ----

static const char *PATH = "romfs:/sample.ogg";  // Path to Ogg Vorbis file to play

static const int THREAD_AFFINITY = 1;           // Execute thread on any core
static const int THREAD_STACK_SZ = 32 * 1024;    // 32kB stack for audio thread

const Thread threadId;

// ---- END DEFINITIONS ----

ndspWaveBuf s_waveBufs[3];
int16_t *s_audioBuffer = NULL;

LightEvent s_event;
volatile bool s_quit = false;  // Quit flag

static OggVorbis_File s_vorbisFile;
static Thread s_threadId;
static bool s_oggLoaded = false;

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

// Pause until user presses a button
void waitForInput(void) {
    printf("Press any button to exit...\n");
    while(aptMainLoop())
    {
        gspWaitForVBlank();
        gfxSwapBuffers();
        hidScanInput();

        if(hidKeysDown())
            break;
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
    linearFree(s_audioBuffer);
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
        if (bytesRead <= 0) {
            if (bytesRead == 0) break;
            printf("ov_read: error %d (%s)", bytesRead, vorbisStrError(bytesRead));
            break;
        }
        totalBytes += bytesRead;
    }

    if (totalBytes == 0) {
        printf("Playback complete\n");
        return false;
    }

    // nsamples is frames, not int16s
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
void audioThread(void *const vorbisFile_) {
    OggVorbis_File *const vorbisFile = (OggVorbis_File *)vorbisFile_;

    while(!s_quit) {  // Whilst the quit flag is unset,
                      // search our waveBufs and fill any that aren't currently
                      // queued for playback (i.e, those that are 'done')
        for(size_t i = 0; i < ARRAY_SIZE(s_waveBufs); ++i) {
            if(s_waveBufs[i].status != NDSP_WBUF_DONE) {
                continue;
            }
            
            if(!fillBuffer(vorbisFile, &s_waveBufs[i])) {   // Playback complete
                return;
            }
        }

        // Wait for a signal that we're needed again before continuing,
        // so that we can yield to other things that want to run
        // (Note that the 3DS uses cooperative threading)
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

static int ogg_mem_close(void *datasource) {
    OggMemoryStream *s = (OggMemoryStream *)datasource;
    free(s->data);
    free(s);
    return 0;
}

bool load_ogg_from_music_library(struct MusicLibrary *library, char *fileName) {
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

    // Only read and close — no seek/tell needed for forward playback
    ov_callbacks cb = {
        .read_func  = ogg_mem_read,
        .seek_func  = NULL,
        .close_func = ogg_mem_close,
        .tell_func  = NULL,
    };

    int error = ov_open_callbacks(stream, &s_vorbisFile, NULL, 0, cb);
    if (error) {
        printf("ov_open_callbacks: %d (%s)\n", error, vorbisStrError(error));
        free(stream->data);
        free(stream);
        return false;
    }

    if (!audioInit(&s_vorbisFile)) {
        ov_clear(&s_vorbisFile);
        return false;
    }

    int32_t priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    priority -= 1;
    if (priority < 0x18) priority = 0x18;
    if (priority > 0x3F) priority = 0x3F;

    s_threadId = threadCreate(audioThread, &s_vorbisFile,
                              THREAD_STACK_SZ, priority,
                              THREAD_AFFINITY, false);
    s_oggLoaded = true;
    return true;
}


void initOggPlayer(void) {
    // Setup LightEvent for synchronisation of audioThread
    LightEvent_Init(&s_event, RESET_ONESHOT);

    // Set the ndsp sound frame callback which signals our audioThread
    ndspSetCallback(audioCallback, NULL);

    // Spawn audio thread

    // Set the thread priority to the main thread's priority ...
    int32_t priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    // ... then subtract 1, as lower number => higher actual priority ...
    priority -= 1;
    // ... finally, clamp it between 0x18 and 0x3F to guarantee that it's valid.
    priority = priority < 0x18 ? 0x18 : priority;
    priority = priority > 0x3F ? 0x3F : priority;
}

void shutdownOggPlayer(void) {
    // Signal audio thread to quit
    s_quit = true;
    LightEvent_Signal(&s_event);

    // Free the audio thread
    threadJoin(threadId, UINT64_MAX);
    threadFree(threadId);

    // Cleanup audio things and de-init platform features
    audioExit();
    if (s_oggLoaded)
    ov_clear(&s_vorbisFile);
}
