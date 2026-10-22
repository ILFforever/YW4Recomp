/* Audio: audren:u (only its audio-device interface) and audout:u (Switchbrew "Audio_services").
 *
 * The game's sound middleware (CRI ADX2) mixes in software and plays plain PCM through
 * AudioOut; the DSP audio renderer is never used. audren:u is opened only for its audio
 * device interface (which output is active, and an event when that changes).
 *
 * AudioOut: a host thread hands each appended buffer's PCM to an SDL3 audio stream once the
 * stream holds less than AUDIO_AHEAD_MS, then releases the buffer and signals the buffer
 * event. So the game's audio thread runs at the rate the host's sound card plays, and the
 * two clocks can't drift apart. Without a sound device (or with YW4_AUDIO=0) the thread
 * "plays" each buffer for as long as its samples last at 48 kHz instead.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "host.h"
#include "ipc_simple.h"
#include "yw4load.h"

#define SAMPLE_RATE 48000
#define FORMAT_PCM16 2
#define MAX_BUFFERS 32
#define AUDOUT_BUFFER_FULL 0x1029u  /* audio (module 153), description 8 */
#define AUDIO_AHEAD_MS 40           /* PCM kept queued in the host stream: latency vs. gaps */

static const SimpleCommand audio_device_simple[] = {
    {12, SIMPLE_EVENT, 0, "QueryAudioDeviceOutputEvent: the output never changes"},
};

static void audio_device(Service *s, IpcRequest *req, IpcResponse *resp) {
    if (!IPC_SIMPLE(audio_device_simple, req, resp)) ipc_unimplemented(s, req, resp);
}

void svc_audren(Service *s, IpcRequest *req, IpcResponse *resp) {
    if (req->cmd == 4) {  /* GetAudioDeviceServiceWithRevisionInfo(aruid, revision) -> IAudioDevice */
        ipc_return_object(resp, ipc_service_new("audren:IAudioDevice", audio_device, NULL));
        return;
    }
    ipc_unimplemented(s, req, resp);
}

/* ---- AudioOut. Its state is shared by IPC handlers and the player thread: both hold the
 * kernel lock while touching it. */

typedef struct {
    uint64_t tag;          /* the guest's AudioOutBuffer address: how the game names a buffer */
    uint64_t samples;      /* frames in it */
    void *pcm;             /* a copy of its PCM16 (NULL without a sound device) */
} QueuedBuffer;

typedef struct {
    uint32_t channels;     /* 2 or 6 (5.1), as the game asked; PCM16 interleaved */
    int started;
    KEvent *event;         /* "a buffer was released" */
    uint32_t event_handle;
    QueuedBuffer queue[MAX_BUFFERS];  /* appended, in play order */
    int nqueue;
    uint64_t released[MAX_BUFFERS];
    int nreleased;
    uint64_t played;       /* frames played so far */
    SDL_AudioStream *stream;  /* the host's sound output; NULL: none, time the buffers instead */
    FILE *dump;               /* YW4_AUDIO_DUMP: raw PCM16 at 48 kHz, as played */
} AudioOut;

/* The host's default sound output as a stream of 48 kHz PCM16 with `channels`; NULL if there
 * is none or YW4_AUDIO=0. */
static SDL_AudioStream *open_stream(uint32_t channels) {
    const char *on = getenv("YW4_AUDIO");
    if (on && *on == '0') return NULL;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "yw4: audout: no sound (%s)\n", SDL_GetError());
        return NULL;
    }
    SDL_AudioSpec spec = {SDL_AUDIO_S16LE, (int)channels, SAMPLE_RATE};  /* 5.1: same channel order as SDL's */
    SDL_AudioStream *s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!s) {
        fprintf(stderr, "yw4: audout: no sound (%s)\n", SDL_GetError());
        return NULL;
    }
    SDL_ResumeAudioStreamDevice(s);
    fprintf(stderr, "yw4: audout: sound on, %u channels\n", channels);
    return s;
}

static void player_thread(void *arg) {
    AudioOut *o = arg;
    int64_t next = host_now_ns();
    const int ahead = SAMPLE_RATE / 1000 * AUDIO_AHEAD_MS * (int)o->channels * 2;  /* bytes */
    for (;;) {
        yw4_kernel_lock();
        int64_t wait_ns = 5000000;  /* idle: look again in 5 ms */
        if (o->started && o->nqueue && o->stream && SDL_GetAudioStreamQueued(o->stream) >= ahead) {
            wait_ns = 2000000;  /* the sound card still has enough: look again in 2 ms */
        } else if (o->started && o->nqueue) {
            QueuedBuffer b = o->queue[0];
            memmove(o->queue, o->queue + 1, (size_t)--o->nqueue * sizeof *o->queue);
            if (o->nreleased < MAX_BUFFERS) o->released[o->nreleased++] = b.tag;
            o->played += b.samples;
            yw4_event_signal(o->event);
            if (o->stream) {
                if (b.pcm) SDL_PutAudioStreamData(o->stream, b.pcm, (int)(b.samples * o->channels * 2));
                if (b.pcm && o->dump) fwrite(b.pcm, o->channels * 2, b.samples, o->dump);
                wait_ns = 0;  /* fill up to AUDIO_AHEAD_MS straight away */
            } else {
                int64_t now = host_now_ns();
                if (next < now - 100000000) next = now;  /* fell behind (or was idle): restart the clock */
                next += (int64_t)(b.samples * 1000000000ull / SAMPLE_RATE);
                wait_ns = next - now;
            }
            free(b.pcm);
        } else {
            next = host_now_ns();
        }
        yw4_kernel_unlock();
        if (wait_ns > 0) yw4_host_sleep_ns(wait_ns);
    }
}

static void audio_out(Service *s, IpcRequest *req, IpcResponse *resp) {
    AudioOut *o = s->state;
    switch (req->cmd) {
    case 0:  /* GetAudioOutState -> 0 started, 1 stopped */
        IPC_OUT(uint32_t, 0) = o->started ? 0 : 1;
        ipc_out_size(resp, 4);
        return;
    case 1:  /* Start */
        o->started = 1;
        return;
    case 2:  /* Stop */
        o->started = 0;
        return;
    case 3: case 7: {  /* AppendAudioOutBuffer(Auto)(u64 tag, buffer: AudioOutBuffer) */
        const IpcBuffer *in = req->cmd == 3 ? &req->a[0] : (req->na && req->a[0].size ? &req->a[0] : &req->x[0]);
        if ((req->cmd == 3 ? req->na : req->na + req->nx) < 1 || in->size < 0x28) break;
        /* AudioOutBuffer: next, sample buffer, capacity, data size, data offset */
        const uint64_t *b = GPTR(in->addr);
        if (o->nqueue == MAX_BUFFERS) {
            resp->result = AUDOUT_BUFFER_FULL;
            return;
        }
        uint64_t samples = b[3] / (o->channels * 2);
        void *pcm = NULL;
        if (o->stream && samples) {  /* copied now: the game may reuse the memory once it is released */
            pcm = malloc(samples * o->channels * 2);
            if (pcm) memcpy(pcm, GPTR(b[1] + b[4]), samples * o->channels * 2);
        }
        o->queue[o->nqueue++] = (QueuedBuffer){IPC_IN(uint64_t, 0), samples, pcm};
        return;
    }
    case 4:  /* RegisterBufferEvent -> event handle */
        resp->copy_handles[resp->ncopy++] = o->event_handle;
        return;
    case 5: case 8: {  /* GetReleasedAudioOutBuffers(Auto)(out: u64 tags[]) -> u32 count */
        IpcBuffer out = req->cmd == 5 ? (req->nb ? req->b[0] : (IpcBuffer){0, 0}) : ipc_out_buffer(req, 0);
        uint32_t n = 0;
        uint64_t *tags = GPTR(out.addr);
        while (n < out.size / 8 && (int)n < o->nreleased) tags[n] = o->released[n], n++;
        memmove(o->released, o->released + n, (size_t)(o->nreleased - (int)n) * 8);
        o->nreleased -= (int)n;
        if (n < out.size / 8) tags[n] = 0;  /* the list ends with a 0 tag if there is room */
        IPC_OUT(uint32_t, 0) = n;
        ipc_out_size(resp, 4);
        return;
    }
    }
    ipc_unimplemented(s, req, resp);
}

/* IAudioOutManager */
void svc_audout(Service *s, IpcRequest *req, IpcResponse *resp) {
    static const char name[] = "DeviceOut";
    switch (req->cmd) {
    case 0: case 2: {  /* ListAudioOuts(Auto)(out: names, 0x100 each) -> u32 count */
        IpcBuffer out = req->cmd == 0 ? (req->nb ? req->b[0] : (IpcBuffer){0, 0}) : ipc_out_buffer(req, 0);
        uint32_t n = out.size >= 0x100;
        if (n) memset(GPTR(out.addr), 0, 0x100), memcpy(GPTR(out.addr), name, sizeof name);
        IPC_OUT(uint32_t, 0) = n;
        ipc_out_size(resp, 4);
        return;
    }
    case 1: case 3: {  /* OpenAudioOut(Auto)(u32 rate, u16 channels, u64 aruid, in name, out name, process) */
        uint32_t rate = IPC_IN(uint32_t, 0);
        uint16_t channels = IPC_IN(uint16_t, 4);
        if (!channels) channels = 2;
        if ((rate && rate != SAMPLE_RATE) || (channels != 2 && channels != 6)) {
            fprintf(stderr, "yw4: audout: %u Hz, %u channels asked; only %u Hz with 2 or 6 channels exists\n", rate,
                    channels, SAMPLE_RATE);
            yw4_stop();
        }
        IpcBuffer out = req->cmd == 1 ? (req->nb ? req->b[0] : (IpcBuffer){0, 0}) : ipc_out_buffer(req, 0);
        if (out.size >= sizeof name) memcpy(GPTR(out.addr), name, sizeof name);
        AudioOut *o = calloc(1, sizeof *o);
        if (!o) exit(3);
        o->channels = channels;
        o->stream = open_stream(channels);
        const char *dump = getenv("YW4_AUDIO_DUMP");
        if (o->stream && dump && *dump) o->dump = fopen(dump, "wb");
        o->event = yw4_event_new(&o->event_handle);
        o->event->host_signalled = 1;
        if (host_thread_start(player_thread, o, 1 << 16)) exit(3);
        IPC_OUT(uint32_t, 0) = SAMPLE_RATE;
        IPC_OUT(uint32_t, 4) = channels;
        IPC_OUT(uint32_t, 8) = FORMAT_PCM16;
        IPC_OUT(uint32_t, 12) = 1;  /* stopped */
        ipc_out_size(resp, 16);
        ipc_return_object(resp, ipc_service_new("audout:IAudioOut", audio_out, o));
        return;
    }
    }
    ipc_unimplemented(s, req, resp);
}
