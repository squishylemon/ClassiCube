#include "../Audio.h"
#include "../Funcs.h"
#include "../Platform.h"
#include "../Errors.h"
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <audsrv.h>

// audsrv only supports a single PCM stream, so all the audio contexts
//  are software mixed together on a separate thread and then sent to audsrv
#define OUTPUT_FREQ     44100
#define MIX_FRAMES      512 // ~11.6 ms per mixed chunk
#define MAX_CONTEXTS    16

struct AudioBuffer {
	cc_int16* samples;
	cc_uint32 frames;
};

struct AudioContext {
	int count; // number of buffers (0 = not initialised)
	int channels, sampleRate, volume;
	cc_bool playing;
	int head, queued;
	cc_uint32 pos, step; // 16.16 fixed point, in frames
	struct AudioBuffer bufs[AUDIO_MAX_BUFFERS];
};
#include "../_AudioBase.h"

static struct AudioContext* active_ctxs[MAX_CONTEXTS];
static int num_active;
static int lock_sema, mix_thread;
static volatile cc_bool mix_stop;

static cc_int32 mix_buf[MIX_FRAMES * 2];
static cc_int16 out_buf[MIX_FRAMES * 2] CC_ALIGNED(64);
static cc_uint8 mix_stack[16 * 1024] CC_ALIGNED(16);

static void Mixer_Lock(void)   { WaitSema(lock_sema);   }
static void Mixer_Unlock(void) { SignalSema(lock_sema); }


/*########################################################################################################################*
*---------------------------------------------------------Mixing----------------------------------------------------------*
*#########################################################################################################################*/
static void MixContext(struct AudioContext* ctx, cc_int32* dst, int frames) {
	int vol = ctx->volume * 256 / 100;
	if (!ctx->playing) return;

	while (frames > 0 && ctx->queued)
	{
		struct AudioBuffer* buf = &ctx->bufs[ctx->head];
		cc_int16* src   = buf->samples;
		cc_uint32 total = buf->frames << 16;
		cc_uint32 last  = buf->frames - 1;

		// Linear interpolation resampling to output frequency
		// (nearest neighbour sounds harsh when upsampling low sample rate music)
		if (ctx->channels == 2) {
			for (; frames > 0 && ctx->pos < total; frames--, dst += 2, ctx->pos += ctx->step)
			{
				cc_uint32 f = ctx->pos >> 16;
				int frac = (ctx->pos & 0xFFFF) >> 1;
				int i = f * 2, j = (f < last ? f + 1 : f) * 2;

				int l = src[i + 0] + (((src[j + 0] - src[i + 0]) * frac) >> 15);
				int r = src[i + 1] + (((src[j + 1] - src[i + 1]) * frac) >> 15);
				dst[0] += (l * vol) >> 8;
				dst[1] += (r * vol) >> 8;
			}
		} else {
			for (; frames > 0 && ctx->pos < total; frames--, dst += 2, ctx->pos += ctx->step)
			{
				cc_uint32 f = ctx->pos >> 16;
				int frac = (ctx->pos & 0xFFFF) >> 1;
				int j = f < last ? f + 1 : f;

				int s = src[f] + (((src[j] - src[f]) * frac) >> 15);
				s = (s * vol) >> 8;
				dst[0] += s;
				dst[1] += s;
			}
		}
		if (ctx->pos < total) break;

		// Finished this buffer, move onto next one
		ctx->pos    -= total;
		ctx->head    = (ctx->head + 1) % ctx->count;
		ctx->queued--;
	}
	if (!ctx->queued) ctx->pos = 0;
}

static void MixChunk(void) {
	Mem_Set(mix_buf, 0, sizeof(mix_buf));

	Mixer_Lock();
	for (int i = 0; i < num_active; i++)
	{
		MixContext(active_ctxs[i], mix_buf, MIX_FRAMES);
	}
	Mixer_Unlock();

	for (int i = 0; i < MIX_FRAMES * 2; i++)
	{
		out_buf[i] = (cc_int16)max(-32768, min(32767, mix_buf[i]));
	}
}

static void MixerThread(void* arg) {
	while (!mix_stop)
	{
		// Blocks (letting other threads run) until audsrv's ring buffer has room
		// NOTE: audsrv_on_fillbuf callbacks were found to stop arriving after a while
		audsrv_wait_audio(sizeof(out_buf));
		if (mix_stop) break;

		MixChunk();
		audsrv_play_audio((const char*)out_buf, sizeof(out_buf));
	}
	ExitDeleteThread();
}


/*########################################################################################################################*
*---------------------------------------------------------Backend---------------------------------------------------------*
*#########################################################################################################################*/
static cc_bool LoadAudioModules(void) {
	extern unsigned char libsd_irx[],  audsrv_irx[];
	extern unsigned int  size_libsd_irx, size_audsrv_irx;
	int ret, res;

	ret = SifExecModuleBuffer(libsd_irx,  size_libsd_irx,  0, NULL, &res);
	if (ret < 0) { Platform_Log1("Failed to load LIBSD: %i", &ret); return false; }

	ret = SifExecModuleBuffer(audsrv_irx, size_audsrv_irx, 0, NULL, &res);
	if (ret < 0) { Platform_Log1("Failed to load AUDSRV: %i", &ret); return false; }
	return true;
}

static int CreateSemaphore(int initial) {
	ee_sema_t sema = { 0 };
	sema.init_count = initial;
	sema.max_count  = 1;
	return CreateSema(&sema);
}

cc_bool AudioBackend_Init(void) {
	struct audsrv_fmt_t fmt;
	ee_thread_status_t info;
	ee_thread_t thread = { 0 };
	int ret;
	// Called by both sounds and music, but the mixer thread must only be started once
	//  (a second one would end up running on the same stack)
	if (mix_thread > 0) return true;

	if (!LoadAudioModules()) return false;
	if ((ret = audsrv_init())) {
		Platform_Log1("audsrv_init failed: %i", &ret); return false;
	}

	fmt.freq     = OUTPUT_FREQ;
	fmt.bits     = 16;
	fmt.channels = 2;
	if ((ret = audsrv_set_format(&fmt))) {
		Platform_Log1("audsrv_set_format failed: %i", &ret); return false;
	}
	audsrv_set_volume(MAX_VOLUME);

	lock_sema = CreateSemaphore(1);
	if (lock_sema < 0) return false;

	// Mixer thread must have higher priority than main thread (lower value = higher priority),
	//  otherwise it would never get to run while the main thread is busy rendering
	// (EE threads aren't preempted by other threads of the same priority)
	ReferThreadStatus(GetThreadId(), &info);
	thread.func             = MixerThread;
	thread.stack            = mix_stack;
	thread.stack_size       = sizeof(mix_stack);
	thread.gp_reg           = &_gp;
	thread.initial_priority = max(0, info.current_priority - 1);

	mix_stop   = false;
	mix_thread = CreateThread(&thread);
	if (mix_thread < 0) return false;
	StartThread(mix_thread, NULL);
	return true;
}

void AudioBackend_Tick(void) { }

void AudioBackend_Free(void) {
	mix_stop   = true;
	mix_thread = 0;
	audsrv_quit();
}


/*########################################################################################################################*
*---------------------------------------------------------Contexts--------------------------------------------------------*
*#########################################################################################################################*/
cc_result Audio_Init(struct AudioContext* ctx, int buffers) {
	Mixer_Lock();
	{
		ctx->count    = buffers;
		ctx->head     = 0;
		ctx->queued   = 0;
		ctx->pos      = 0;
		ctx->step     = 1 << 16;
		ctx->volume   = 100;
		ctx->playing  = false;
		ctx->channels = 1;

		int i;
		for (i = 0; i < num_active; i++)
		{
			if (active_ctxs[i] == ctx) break;
		}
		if (i == num_active && num_active < MAX_CONTEXTS) active_ctxs[num_active++] = ctx;
	}
	Mixer_Unlock();
	return 0;
}

void Audio_Close(struct AudioContext* ctx) {
	Mixer_Lock();
	{
		for (int i = 0; i < num_active; i++)
		{
			if (active_ctxs[i] != ctx) continue;
			active_ctxs[i] = active_ctxs[--num_active];
			break;
		}
		ctx->count   = 0;
		ctx->queued  = 0;
		ctx->playing = false;
	}
	Mixer_Unlock();
}

cc_result Audio_SetFormat(struct AudioContext* ctx, int channels, int sampleRate, int playbackRate) {
	sampleRate = Audio_AdjustSampleRate(sampleRate, playbackRate);

	Mixer_Lock();
	{
		ctx->channels   = channels;
		ctx->sampleRate = sampleRate;
		ctx->step       = (cc_uint32)(((cc_uint64)sampleRate << 16) / OUTPUT_FREQ);
	}
	Mixer_Unlock();
	return 0;
}

void Audio_SetVolume(struct AudioContext* ctx, int volume) {
	ctx->volume = volume;
}

cc_result Audio_QueueChunk(struct AudioContext* ctx, struct AudioChunk* chunk) {
	cc_result res = 0;

	Mixer_Lock();
	{
		if (ctx->queued >= ctx->count) {
			// tried to queue data without polling for free buffers first
			res = ERR_INVALID_ARGUMENT;
		} else {
			struct AudioBuffer* buf = &ctx->bufs[(ctx->head + ctx->queued) % ctx->count];
			buf->samples = (cc_int16*)chunk->data;
			buf->frames  = chunk->size / (2 * ctx->channels);
			ctx->queued++;
		}
	}
	Mixer_Unlock();
	return res;
}

cc_result Audio_Play(struct AudioContext* ctx) {
	ctx->playing = true;
	return 0;
}

cc_result Audio_Poll(struct AudioContext* ctx, int* inUse) {
	*inUse = ctx->queued;
	return 0;
}


/*########################################################################################################################*
*------------------------------------------------------Stream context-----------------------------------------------------*
*#########################################################################################################################*/
cc_result StreamContext_SetFormat(struct AudioContext* ctx, int channels, int sampleRate, int playbackRate) {
	return Audio_SetFormat(ctx, channels, sampleRate, playbackRate);
}

cc_result StreamContext_Enqueue(struct AudioContext* ctx, struct AudioChunk* chunk) {
	return Audio_QueueChunk(ctx, chunk);
}

cc_result StreamContext_Play(struct AudioContext* ctx) {
	return Audio_Play(ctx);
}

cc_result StreamContext_Pause(struct AudioContext* ctx) {
	ctx->playing = false;
	return 0;
}

cc_result StreamContext_Update(struct AudioContext* ctx, int* inUse) {
	return Audio_Poll(ctx, inUse);
}


/*########################################################################################################################*
*------------------------------------------------------Sound context------------------------------------------------------*
*#########################################################################################################################*/
cc_bool SoundContext_FastPlay(struct AudioContext* ctx, struct AudioData* data) {
	return true;
}

cc_result SoundContext_PlayData(struct AudioContext* ctx, struct AudioData* data) {
	cc_result res;

	if ((res = Audio_SetFormat(ctx,  data->channels, data->sampleRate, data->rate))) return res;
	if ((res = Audio_QueueChunk(ctx, &data->chunk))) return res;
	if ((res = Audio_Play(ctx))) return res;

	return 0;
}

cc_result SoundContext_PollBusy(struct AudioContext* ctx, cc_bool* isBusy) {
	int inUse = 1;
	cc_result res;
	if ((res = Audio_Poll(ctx, &inUse))) return res;

	*isBusy = inUse > 0;
	return 0;
}


/*########################################################################################################################*
*--------------------------------------------------------Audio misc-------------------------------------------------------*
*#########################################################################################################################*/
cc_bool Audio_DescribeError(cc_result res, cc_string* dst) {
	return false;
}
