#include "../Audio.h"
#include "../Funcs.h"
#include "../Platform.h"
#include "../Errors.h"
#include "../Utils.h"
#include <stdio.h>
#include <libdragon.h>

// All the audio contexts are mixed together in software on a separate thread, and then sent to the AI
// NOTE: libdragon's mixer can't be used, as it runs on the RSP through the same command queue that
//  rendering uses, which isn't safe to use from more than one thread
#define OUTPUT_FREQ  32000 // ClassiCube's sounds are 32 kHz, so they don't need resampling
#define MAX_CONTEXTS 16

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
#define AUDIO_OVERRIDE_SOUNDS
#include "../_AudioBase.h"

static struct AudioContext* active_ctxs[MAX_CONTEXTS];
static int num_active;
static kmutex_t mix_lock;
static kthread_t* mix_thread;
static volatile cc_bool mix_stop, mix_finished;
static cc_int32* mix_buf;

static void Mixer_Lock(void)   { kmutex_lock(&mix_lock);   }
static void Mixer_Unlock(void) { kmutex_unlock(&mix_lock); }


/*########################################################################################################################*
*---------------------------------------------------------Mixing----------------------------------------------------------*
*#########################################################################################################################*/
// Sounds are stored uncompressed in the ROM filesystem, and played directly from there (see
//  AudioBackend_LoadSounds), so that they don't use up any RAM. For such sounds, the samples
//  pointer is the ROM address of the samples (which are below 0x80000000, unlike RAM addresses)
#define IsRomSamples(samples) ((cc_uintptr)(samples) < 0x80000000)
// Number of sample frames that can be read from ROM at once (enough for 256 output frames,
//  even when resampling a sound that's played at up to 4 times the output frequency)
#define ROM_MIX_FRAMES 256
#define ROM_BUF_FRAMES (ROM_MIX_FRAMES * 4 + 2)
static cc_int16 rom_buf[ROM_BUF_FRAMES * 2] __attribute__((aligned(16)));

// Reads the given range of sample frames from ROM into rom_buf
static void ReadRomSamples(struct AudioBuffer* buf, int channels, cc_uint32 first, cc_uint32 count) {
	cc_uint32 offset = first * channels * 2;
	cc_uint32 size   = count  * channels * 2;

	// NOTE: DMA doesn't go through the CPU's data cache
	data_cache_hit_writeback_invalidate(rom_buf, CC_ALIGNUP(size, 16));
	dma_read(rom_buf, (cc_uintptr)buf->samples + offset, size);

	// WAV samples are little endian
	Utils_SwapEndian16(rom_buf, count * channels);
}

// Mixes up to the given number of frames from the samples in src, which start at the given
//  frame of the buffer, and returns how many frames were mixed
static int MixFrames(struct AudioContext* ctx, struct AudioBuffer* buf, const cc_int16* src, cc_uint32 base,
					cc_int32* dst, int frames) {
	int vol = ctx->volume * 256 / 100;
	cc_uint32 total = buf->frames << 16;
	cc_uint32 last  = buf->frames - 1;
	int mixed = 0;

	if (ctx->step == (1 << 16) && ctx->channels == 1) {
		// Fast path for sounds that are already at the output frequency
		cc_uint32 f = ctx->pos >> 16;
		for (; mixed < frames && f < buf->frames; mixed++, dst += 2, f++)
		{
			int s = (src[f - base] * vol) >> 8;
			dst[0] += s;
			dst[1] += s;
		}
		ctx->pos = f << 16;
	} else if (ctx->channels == 2) {
		// Linear interpolation resampling to output frequency
		// (nearest neighbour sounds harsh when upsampling low sample rate music)
		for (; mixed < frames && ctx->pos < total; mixed++, dst += 2, ctx->pos += ctx->step)
		{
			cc_uint32 f = ctx->pos >> 16;
			int frac = (ctx->pos & 0xFFFF) >> 1;
			int i = (f - base) * 2, j = ((f < last ? f + 1 : f) - base) * 2;

			int l = src[i + 0] + (((src[j + 0] - src[i + 0]) * frac) >> 15);
			int r = src[i + 1] + (((src[j + 1] - src[i + 1]) * frac) >> 15);
			dst[0] += (l * vol) >> 8;
			dst[1] += (r * vol) >> 8;
		}
	} else {
		for (; mixed < frames && ctx->pos < total; mixed++, dst += 2, ctx->pos += ctx->step)
		{
			cc_uint32 f = ctx->pos >> 16;
			int frac = (ctx->pos & 0xFFFF) >> 1;
			int i = f - base, j = (f < last ? f + 1 : f) - base;

			int s = src[i] + (((src[j] - src[i]) * frac) >> 15);
			s = (s * vol) >> 8;
			dst[0] += s;
			dst[1] += s;
		}
	}
	return mixed;
}

static void MixContext(struct AudioContext* ctx, cc_int32* dst, int frames) {
	if (!ctx->playing) return;

	while (frames > 0 && ctx->queued)
	{
		struct AudioBuffer* buf = &ctx->bufs[ctx->head];
		cc_uint32 total = buf->frames << 16;
		int mixed;

		if (IsRomSamples(buf->samples)) {
			// Only read the samples that will be needed for the next few output frames
			int count = min(frames, ROM_MIX_FRAMES);
			cc_uint32 first = ctx->pos >> 16;
			cc_uint32 end   = (cc_uint32)((ctx->pos + (cc_uint64)ctx->step * count) >> 16) + 2;

			if (end > first + ROM_BUF_FRAMES) {
				// Playing at a very high frequency, so mix fewer frames to stay within the samples read
				end   = first + ROM_BUF_FRAMES;
				count = max(1, (int)(((cc_uint64)(ROM_BUF_FRAMES - 2) << 16) / ctx->step));
			}
			end = min(end, buf->frames);

			ReadRomSamples(buf, ctx->channels, first, end - first);
			mixed = MixFrames(ctx, buf, rom_buf, first, dst, count);
		} else {
			mixed = MixFrames(ctx, buf, buf->samples, 0, dst, frames);
		}

		dst    += mixed * 2;
		frames -= mixed;
		if (ctx->pos < total) {
			if (mixed) continue;
			break;
		}

		// Finished this buffer, move onto next one
		ctx->pos    -= total;
		ctx->head    = (ctx->head + 1) % ctx->count;
		ctx->queued--;
	}
	if (!ctx->queued) ctx->pos = 0;
}

static void MixChunk(cc_int16* out, int frames) {
	Mem_Set(mix_buf, 0, frames * 2 * sizeof(cc_int32));

	Mixer_Lock();
	for (int i = 0; i < num_active; i++)
	{
		MixContext(active_ctxs[i], mix_buf, frames);
	}
	Mixer_Unlock();

	for (int i = 0; i < frames * 2; i++)
	{
		out[i] = (cc_int16)max(-32768, min(32767, mix_buf[i]));
	}
}

static int MixerThread(void* arg) {
	int frames = audio_get_buffer_length();
	kirq_wait_t wait = kirq_begin_wait_ai();

	while (!mix_stop)
	{
		// The AI interrupt is raised whenever the AI starts playing the next buffer, which
		//  frees up a buffer to mix into. Mixer thread has a higher priority than the main
		//  thread, so waiting here lets the main thread run until then
		if (!audio_can_write()) { kirq_wait(&wait); continue; }

		cc_int16* out = audio_write_begin();
		MixChunk(out, frames);
		audio_write_end();
	}
	mix_finished = true;
	return 0;
}


/*########################################################################################################################*
*---------------------------------------------------------Backend---------------------------------------------------------*
*#########################################################################################################################*/
// Must be higher than the music thread's priority, as decoding music can take a while
#define MIXER_THREAD_PRIORITY 2

cc_bool AudioBackend_Init(void) {
	// Called by both sounds and music, but the mixer thread must only be started once
	if (mix_thread) return true;

	audio_init(OUTPUT_FREQ, AUDIO_INIT_LATENCY_MS(100));
	mix_buf = (cc_int32*)Mem_TryAlloc(audio_get_buffer_length() * 2, sizeof(cc_int32));
	if (!mix_buf) { audio_close(); return false; }

	kmutex_init(&mix_lock, KMUTEX_STANDARD);
	mix_stop     = false;
	mix_finished = false;
	mix_thread   = kthread_new("Mixer", 8 * 1024, MIXER_THREAD_PRIORITY, MixerThread, NULL);
	return mix_thread != NULL;
}

void AudioBackend_Tick(void) { }

void AudioBackend_Free(void) {
	kirq_wait_t wait = kirq_begin_wait_vi();
	if (!mix_thread) return;
	mix_stop = true;

	// libdragon fails an assertion when joining a thread that hasn't finished yet (see Thread_Join),
	//  so wait for it to finish first (mixer thread wakes up on the next AI interrupt)
	while (!mix_finished) kirq_wait(&wait);
	kthread_join(mix_thread);
	mix_thread = NULL;

	audio_close();
	Mem_Free(mix_buf);
	mix_buf = NULL;
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


/*########################################################################################################################*
*---------------------------------------------------------Sounds----------------------------------------------------------*
*#########################################################################################################################*/
static cc_uint8 wav_hdr[16] __attribute__((aligned(16)));

static void ReadWavHeader(pi_addr_t addr, int size) {
	// NOTE: DMA doesn't go through the CPU's data cache
	data_cache_hit_writeback_invalidate(wav_hdr, sizeof(wav_hdr));
	dma_read(wav_hdr, addr, size);
}

// Finds the format and samples of a .wav file in the ROM filesystem, and adds it to the group
static cc_bool LoadRomSound(struct SoundGroup* group, const char* path) {
	pi_addr_t rom    = dfs_rom_addr(path);
	cc_uint32 len    = dfs_rom_size(path);
	cc_uint32 offset = 12; // skip RIFF header
	int channels = 0, sampleRate = 0, bitsPerSample = 0;
	if (!rom || group->count == AUDIO_MAX_SOUNDS) return false;

	while (offset + 8 <= len)
	{
		ReadWavHeader(rom + offset, 8);
		cc_uint32 fourCC = Mem_ReadU32_BE(wav_hdr);
		cc_uint32 size   = Mem_ReadU32_LE(wav_hdr + 4);
		offset += 8;

		if (fourCC == 0x666D7420) { // "fmt "
			ReadWavHeader(rom + offset, 16);
			channels      = Mem_ReadU16_LE(wav_hdr + 2);
			sampleRate    = Mem_ReadU32_LE(wav_hdr + 4);
			bitsPerSample = Mem_ReadU16_LE(wav_hdr + 14);
		} else if (fourCC == 0x64617461) { // "data"
			if (bitsPerSample != 16 || channels < 1 || channels > 2 || !sampleRate) return false;

			struct Sound* snd = &group->sounds[group->count++];
			snd->channels   = channels;
			snd->sampleRate = sampleRate;
			snd->chunk.data = (void*)(cc_uintptr)(rom + offset);
			snd->chunk.size = min(size, len - offset);
			return true;
		}
		offset += size + (size & 1);
	}
	return false;
}

static void LoadRomSounds(struct Soundboard* board, const char* boardName) {
	char path[64];

	for (int type = SOUND_NONE + 1; type < SOUND_COUNT; type++)
	{
		for (int i = 1; i <= AUDIO_MAX_SOUNDS; i++)
		{
			snprintf(path, sizeof(path), "sounds/%s_%s%i.wav", boardName, Sound_Names[type], i);
			if (!LoadRomSound(&board->groups[type], path)) break;
		}
	}
}

void AudioBackend_LoadSounds(void) {
	cc_filepath mcPath, ccPath;
	Platform_EncodePath(&mcPath, &Sounds_ZipPathMC);
	Platform_EncodePath(&ccPath, &Sounds_ZipPathCC);

	// Sounds put on the SD card take priority over ClassiCube's sounds in the ROM
	if (File_Exists(&mcPath) || File_Exists(&ccPath)) {
		Sounds_LoadDefault();
	} else {
		LoadRomSounds(&digBoard,  "dig");
		LoadRomSounds(&stepBoard, "step");
	}
}
