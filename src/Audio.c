#include "Audio.h"
#include "String_.h"
#include "Logger.h"
#include "Event.h"
#include "Block.h"
#include "ExtMath.h"
#include "Funcs.h"
#include "Game.h"
#include "Errors.h"
#include "Vorbis.h"
#include "Chat.h"
#include "Stream.h"
#include "Utils.h"
#include "Options.h"
#include "Deflate.h"
#ifdef CC_BUILD_MOBILE
/* TODO: Refactor maybe to not rely on checking WinInfo.Handle != NULL */
#include "Window.h"
#endif

int Audio_SoundsVolume, Audio_MusicVolume;

const char* const Sound_Names[SOUND_COUNT] = {
	"none", "wood", "gravel", "grass", "stone",
	"metal", "glass", "cloth", "sand", "snow",
};

#ifdef CC_BIG_ENDIAN
/* Hacky workaround to ensure that sounds are re-fetched on big endian systems, */
/*  as the .wav writing code at one point accidentally wrote big endian instead */ 
/*  of little endian samples to the .wav file */
const cc_string Sounds_ZipPathMC = String_FromConst("audio/default2.zip");
#else
const cc_string Sounds_ZipPathMC = String_FromConst("audio/default.zip");
#endif
const cc_string Sounds_ZipPathCC = String_FromConst("audio/classicube.zip");
static const cc_string audio_dir = String_FromConst("audio");


/*########################################################################################################################*
*--------------------------------------------------------Sounds-----------------------------------------------------------*
*#########################################################################################################################*/
#ifdef CC_BUILD_NOSOUNDS
/* Can't use mojang's sound assets, so just stub everything out */
static void Sounds_Init(void) { }
static void Sounds_Free(void) { }
static void Sounds_Stop(void) { }
static void Sounds_Start(void) {
	Chat_AddRaw("&cSounds are not supported currently");
	Audio_SoundsVolume = 0;
}

void Audio_PlayDigSound(cc_uint8 type)  { }
void Audio_PlayStepSound(cc_uint8 type) { }

void Sounds_LoadDefault(void) { }
#else
struct Soundboard digBoard, stepBoard;
static RNGState sounds_rnd;

#define WAV_FourCC(a, b, c, d) (((cc_uint32)a << 24) | ((cc_uint32)b << 16) | ((cc_uint32)c << 8) | (cc_uint32)d)
#define WAV_FMT_SIZE 16

static cc_result Sound_ReadWaveData(struct Stream* stream, struct Sound* snd) {
	cc_uint32 fourCC, size;
	cc_uint8 tmp[WAV_FMT_SIZE];
	cc_result res;
	int bitsPerSample;

	if ((res = Stream_Read(stream, tmp, 12)))  return res;
	fourCC = Mem_ReadU32_BE(tmp + 0);
	if (fourCC == WAV_FourCC('I','D','3', 2))  return AUDIO_ERR_MP3_SIG; /* ID3 v2.2 tags header */
	if (fourCC == WAV_FourCC('I','D','3', 3))  return AUDIO_ERR_MP3_SIG; /* ID3 v2.3 tags header */
	if (fourCC != WAV_FourCC('R','I','F','F')) return WAV_ERR_STREAM_HDR;

	/* tmp[4] (4) file size */
	fourCC = Mem_ReadU32_BE(tmp + 8);
	if (fourCC != WAV_FourCC('W','A','V','E')) return WAV_ERR_STREAM_TYPE;

	for (;;) {
		if ((res = Stream_Read(stream, tmp, 8))) return res;
		fourCC = Mem_ReadU32_BE(tmp + 0);
		size   = Mem_ReadU32_LE(tmp + 4);

		if (fourCC == WAV_FourCC('f','m','t',' ')) {
			if ((res = Stream_Read(stream, tmp, sizeof(tmp)))) return res;
			if (Mem_ReadU16_LE(tmp + 0) != 1) return WAV_ERR_DATA_TYPE;

			snd->channels   = Mem_ReadU16_LE(tmp + 2);
			snd->sampleRate = Mem_ReadU32_LE(tmp + 4);
			/* tmp[8] (6) alignment data and stuff */

			bitsPerSample = Mem_ReadU16_LE(tmp + 14);
			if (bitsPerSample != 16) return WAV_ERR_SAMPLE_BITS;
			size -= WAV_FMT_SIZE;
		} else if (fourCC == WAV_FourCC('d','a','t','a')) {
			if ((res = Audio_AllocChunks(size, &snd->chunk, 1))) return res;
			res = Stream_Read(stream, (cc_uint8*)snd->chunk.data, size);

			#ifdef CC_BIG_ENDIAN
			Utils_SwapEndian16((cc_int16*)snd->chunk.data, size / 2);
			#endif
			return res;
		}

		/* Skip over unhandled data */
		if (size && (res = stream->Skip(stream, size))) return res;
	}
}

static struct SoundGroup* Soundboard_FindGroup(struct Soundboard* board, const cc_string* name) {
	struct SoundGroup* groups = board->groups;
	int i;

	for (i = 0; i < SOUND_COUNT; i++) 
	{
		if (String_CaselessEqualsConst(name, Sound_Names[i])) return &groups[i];
	}
	return NULL;
}

static void Soundboard_Load(struct Soundboard* board, const cc_string* boardName, const cc_string* file, struct Stream* stream) {
	struct SoundGroup* group;
	struct Sound* snd;
	cc_string name = *file;
	cc_result res;
	int dotIndex;
	Utils_UNSAFE_TrimFirstDirectory(&name);

	/* dig_grass1.wav -> dig_grass1 */
	dotIndex = String_LastIndexOf(&name, '.');
	if (dotIndex >= 0) name.length = dotIndex;
	if (!String_CaselessStarts(&name, boardName)) return;

	/* Convert dig_grass1 to grass */
	name = String_UNSAFE_SubstringAt(&name, boardName->length);
	name = String_UNSAFE_Substring(&name, 0, name.length - 1);

	group = Soundboard_FindGroup(board, &name);
	if (!group) {
		Chat_Add1("&cUnknown sound group '%s'", &name); return;
	}
	if (group->count == Array_Elems(group->sounds)) {
		Chat_AddRaw("&cCannot have more than 10 sounds in a group"); return;
	}

	snd = &group->sounds[group->count];
	res = Sound_ReadWaveData(stream, snd);

	if (res) {
		Logger_SysWarn2(res, "decoding", file);
		Audio_FreeChunks(&snd->chunk, 1);
		snd->chunk.data = NULL;
		snd->chunk.size = 0;
	} else { group->count++; }
}

static const struct Sound* Soundboard_PickRandom(struct Soundboard* board, cc_uint8 type) {
	struct SoundGroup* group;
	int idx;

	if (type == SOUND_NONE || type >= SOUND_COUNT) return NULL;
	if (type == SOUND_METAL) type = SOUND_STONE;

	group = &board->groups[type];
	if (!group->count) return NULL;

	idx = Random_Next(&sounds_rnd, group->count);
	return &group->sounds[idx];
}


CC_NOINLINE static void Sounds_Fail(cc_result res) {
	Audio_Warn(res, "playing sounds");
	Chat_AddRaw("&cDisabling sounds");
	Audio_SetSounds(0);
}

static void Sounds_Play(cc_uint8 type, struct Soundboard* board) {
	const struct Sound* snd;
	struct AudioData data;
	cc_result res;

	if (type == SOUND_NONE || !Audio_SoundsVolume) return;
	snd = Soundboard_PickRandom(board, type);
	if (!snd) return;

	data.chunk      = snd->chunk;
	data.channels   = snd->channels;
	data.sampleRate = snd->sampleRate;
	data.rate       = 100;
	data.volume     = Audio_SoundsVolume;

	/* https://minecraft.wiki/w/Block_of_Gold#Sounds */
	/* https://minecraft.wiki/w/Grass#Sounds */
	if (board == &digBoard) {
		if (type == SOUND_METAL) data.rate = 120;
		else data.rate = 80;
	} else {
		data.volume /= 2;
		if (type == SOUND_METAL) data.rate = 140;
	}
	
	res = AudioPool_Play(&data);
	if (res) Sounds_Fail(res);
}

static void Audio_PlayBlockSound(void* obj, IVec3 coords, BlockID old, BlockID now) {
	if (now == BLOCK_AIR) {
		Audio_PlayDigSound(Blocks.DigSounds[old]);
	} else if (!Game_ClassicMode) {
		/* use StepSounds instead when placing, as don't want */
		/*  to play glass break sound when placing glass */
		Audio_PlayDigSound(Blocks.StepSounds[now]);
	}
}

#if defined CC_BUILD_BUNDLED_SOUNDS && defined CC_BUNDLED_FILES_DIR
/* ClassiCube's own sounds, stored alongside the game (e.g. in the ROM) */
static cc_result Sounds_OpenBundled(struct Stream* stream) {
	static const cc_string bundled = String_FromConst(CC_BUNDLED_FILES_DIR "cc_audio.zip");
	cc_filepath raw_path;

	Platform_EncodePath(&raw_path, &bundled);
	return Stream_OpenPath(stream, &raw_path);
}
#elif defined CC_BUILD_BUNDLED_SOUNDS
/* ClassiCube's own sounds */
CC_EMBED_FILE(ccBundledSounds, "misc/cc_audio.zip")

static cc_result Sounds_OpenBundled(struct Stream* stream) {
	Stream_ReadonlyMemory(stream, (void*)ccBundledSounds,
						(cc_uint32)(ccBundledSoundsEnd - ccBundledSounds));
	return 0;
}
#endif

static cc_bool SelectZipEntry(const cc_string* path) { return true; }
static cc_result ProcessZipEntry(const cc_string* path, struct Stream* stream, struct ZipEntry* source) {
	static const cc_string dig  = String_FromConst("dig_");
	static const cc_string step = String_FromConst("step_");
	
	Soundboard_Load(&digBoard,  &dig,  path, stream);
	Soundboard_Load(&stepBoard, &step, path, stream);
	return 0;
}

static cc_result Sounds_ExtractZip(const cc_string* path) {
	struct ZipEntry entries[128];
	struct Stream stream;
	cc_filepath raw_path;
	cc_result res;

	Platform_EncodePath(&raw_path, path);
	res = Stream_OpenPath(&stream, &raw_path);
#ifdef CC_BUILD_BUNDLED_SOUNDS
	/* Fall back to ClassiCube sounds built into the executable, e.g. when no network to download them */
	if (ReturnCode_IsNotFound(res) && String_Equals(path, &Sounds_ZipPathCC)) {
		res = Sounds_OpenBundled(&stream);
	}
	/* Original sounds not being downloaded is normal, as ClassiCube sounds are used instead */
	if (ReturnCode_IsNotFound(res) && String_Equals(path, &Sounds_ZipPathMC)) return res;
#endif
	if (res) { Logger_IOWarn2(res, "opening", &raw_path); return res; }

	res = Zip_Extract(&stream, SelectZipEntry, ProcessZipEntry,
						entries, Array_Elems(entries));
	if (res) Logger_IOWarn2(res, "extracting", &raw_path);

	/* No point logging error for closing readonly file */
	(void)stream.Close(&stream);
	return res;
}

void Sounds_LoadDefault(void) {
	cc_result res = Sounds_ExtractZip(&Sounds_ZipPathMC);
	if (res == ReturnCode_FileNotFound)
		Sounds_ExtractZip(&Sounds_ZipPathCC);
}

static cc_bool sounds_loaded;
static void Sounds_Start(void) {
	if (!AudioBackend_Init()) { 
		AudioBackend_Free(); 
		Audio_SoundsVolume = 0; 
		return; 
	}

	if (sounds_loaded) return;
	sounds_loaded = true;
	AudioBackend_LoadSounds();
}

static void Sounds_Stop(void) { AudioPool_Close(); }

static void Sounds_Init(void) {
	int volume = Options_GetInt(OPT_SOUND_VOLUME, 0, 100, DEFAULT_SOUNDS_VOLUME);
	Audio_SetSounds(volume);
	Event_Register_(&UserEvents.BlockChanged, NULL, Audio_PlayBlockSound);
}
static void Sounds_Free(void) { Sounds_Stop(); }

void Audio_PlayDigSound(cc_uint8 type)  { Sounds_Play(type, &digBoard); }
void Audio_PlayStepSound(cc_uint8 type) { Sounds_Play(type, &stepBoard); }
#endif


#ifdef CC_BUILD_COMPACTMUSIC
/*########################################################################################################################*
*--------------------------------------------------------IMA ADPCM--------------------------------------------------------*
*#########################################################################################################################*/
static const cc_int16 adpcm_steps[89] = {
	    7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
	   19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
	   50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
	  130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
	  337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
	  876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
	 2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
	 5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
	15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static const cc_int8 adpcm_indices[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

static void Adpcm_Update(struct AdpcmState* s, int nibble, int delta) {
	int pred  = s->predictor + ((nibble & 8) ? -delta : delta);
	int index = s->index + adpcm_indices[nibble & 7];

	s->predictor = max(-32768, min(32767, pred));
	s->index     = max(0, min(88, index));
}

int Adpcm_EncodeSample(struct AdpcmState* s, int sample) {
	int step   = adpcm_steps[s->index];
	int diff   = sample - s->predictor;
	int delta  = step >> 3;
	int nibble = 0;

	if (diff < 0) { nibble = 8; diff = -diff; }
	if (diff >= step) { nibble |= 4; diff -= step; delta += step; }
	step >>= 1;
	if (diff >= step) { nibble |= 2; diff -= step; delta += step; }
	step >>= 1;
	if (diff >= step) { nibble |= 1; delta += step; }

	/* Track the predictor exactly as the decoder will */
	Adpcm_Update(s, nibble, delta);
	return nibble;
}

int Adpcm_DecodeSample(struct AdpcmState* s, int nibble) {
	int step  = adpcm_steps[s->index];
	int delta = step >> 3;

	if (nibble & 4) delta += step;
	if (nibble & 2) delta += step >> 1;
	if (nibble & 1) delta += step >> 2;

	Adpcm_Update(s, nibble, delta);
	return s->predictor;
}
#endif


/*########################################################################################################################*
*--------------------------------------------------------Music------------------------------------------------------------*
*#########################################################################################################################*/
#ifdef CC_BUILD_NOMUSIC
/* Can't use mojang's music assets, so just stub everything out */
static void Music_Init(void) { }
static void Music_Free(void) { }
static void Music_Stop(void) { }
static void Music_Start(void) {
	Chat_AddRaw("&cMusic is not supported currently");
	Audio_MusicVolume = 0;
}
#else
static void* music_thread;
static void* music_waitable;
static volatile cc_bool music_stopping, music_joining;
static int music_minDelay, music_maxDelay;

/* Decodes the next frame of audio, outputting the number of samples decoded */
typedef cc_result (*Music_DecodeFrame)(void* obj, cc_int16* data, int* samples);

static cc_result Music_Buffer(struct AudioChunk* chunk, int maxSamples, Music_DecodeFrame decode, void* obj) {
	int samples = 0, count;
	cc_result res = 0, res2;
	cc_int16* data = (cc_int16*)chunk->data;

	while (samples < maxSamples) {
		if ((res = decode(obj, &data[samples], &count))) break;
		samples += count;
	}

	chunk->size = samples * 2;
	res2 = StreamContext_Enqueue(&music_ctx, chunk);
	if (res2) { music_stopping = true; return res2; }
	return res;
}

/* Plays audio from the given decoder, until either the end of the audio is reached or music is stopped */
/* frameSamples is the maximum number of samples that a single decoded frame can contain */
static cc_result Music_Stream(int channels, int sampleRate, int frameSamples, Music_DecodeFrame decode, void* obj) {
	int volume, chunkSize, samplesPerSecond;
	struct AudioChunk chunks[AUDIO_MAX_BUFFERS] = { 0 };
	int inUse, i, cur;
	cc_result res;

	if ((res = StreamContext_SetFormat(&music_ctx, channels, sampleRate, 100))) return res;

	/* the last frame can end up going over, */
	/*  so can end up decoding slightly over a second of audio */
	chunkSize        = channels * sampleRate + frameSamples;
	samplesPerSecond = channels * sampleRate;

	if ((res = Audio_AllocChunks(chunkSize * 2, chunks, AUDIO_MAX_BUFFERS))) goto cleanup;
    volume = Audio_MusicVolume;
    Audio_SetVolume(&music_ctx, volume);	

	/* fill up with some samples before playing */
	for (i = 0; i < AUDIO_MAX_BUFFERS && !res; i++) 
	{
		res = Music_Buffer(&chunks[i], samplesPerSecond, decode, obj);
	}
	if (music_stopping) goto cleanup;

	res  = StreamContext_Play(&music_ctx);
	if (res) goto cleanup;
	cur  = 0;

	while (!music_stopping) {
#ifdef CC_BUILD_MOBILE
		/* Don't play music while in the background on Android */
    	/* TODO: Not use such a terrible approach */
    	if (!Window_Main.Handle.ptr || Window_Main.Inactive) {
    		StreamContext_Pause(&music_ctx);
    		while ((!Window_Main.Handle.ptr || Window_Main.Inactive) && !music_stopping) {
    			Thread_Sleep(10); continue;
    		}
    		StreamContext_Play(&music_ctx);
    	}
#endif
        if (volume != Audio_MusicVolume) {
            volume = Audio_MusicVolume;
            Audio_SetVolume(&music_ctx, volume);
        }

		res = StreamContext_Update(&music_ctx, &inUse);
		if (res) { music_stopping = true; break; }

		if (inUse >= AUDIO_MAX_BUFFERS) {
			Thread_Sleep(10); continue;
		}

		res = Music_Buffer(&chunks[cur], samplesPerSecond, decode, obj);
		cur = (cur + 1) % AUDIO_MAX_BUFFERS;

		/* need to specially handle last bit of audio */
		if (res) break;
	}

	if (music_stopping) {
		/* must close audio context, as otherwise some of the audio */
		/*  context's internal audio buffers may have a reference */
		/*  to the `data` buffer which will be freed after this */
		Audio_Close(&music_ctx);
	} else {
		/* Wait until the buffers finished playing */
		for (;;) {
			if (StreamContext_Update(&music_ctx, &inUse) || inUse == 0) break;
			Thread_Sleep(10);
		}
	}

cleanup:
	Audio_FreeChunks(chunks, AUDIO_MAX_BUFFERS);
	return res;
}

static cc_result Music_DecodeVorbis(void* obj, cc_int16* data, int* samples) {
	struct VorbisState* ctx = (struct VorbisState*)obj;
	cc_result res = Vorbis_DecodeFrame(ctx);
	if (res) return res;

	*samples = Vorbis_OutputFrame(ctx, data);
	return 0;
}

static cc_result Music_PlayOgg(struct Stream* source) {
	cc_result res;
#if CC_BUILD_MAXSTACK <= (64 * 1024)
	struct VorbisState* vorbis = (struct VorbisState*)Mem_TryAllocCleared(1, sizeof(struct VorbisState));
	struct OggState* ogg = (struct OggState*)Mem_TryAllocCleared(1, sizeof(struct OggState));
	if (!vorbis || !ogg) { Mem_Free(vorbis); Mem_Free(ogg); return ERR_OUT_OF_MEMORY; }
#else
	struct OggState _ogg;
	struct OggState* ogg = &_ogg;
	struct VorbisState _vorbis;
	struct VorbisState* vorbis = &_vorbis;
#endif

	Ogg_Init(ogg, source);
	Vorbis_Init(vorbis);
	vorbis->source = ogg;

	res = Vorbis_DecodeHeaders(vorbis);
	if (!res) res = Music_Stream(vorbis->channels, vorbis->sampleRate,
								vorbis->blockSizes[1] * vorbis->channels, Music_DecodeVorbis, vorbis);

	Vorbis_Free(vorbis);
#if CC_BUILD_MAXSTACK <= (64 * 1024)
	Mem_Free(ogg);
	Mem_Free(vorbis);
#endif
	return res == ERR_END_OF_STREAM ? 0 : res;
}

#ifdef CC_BUILD_COMPACTMUSIC
#define WAV_FourCC(a, b, c, d) (((cc_uint32)a << 24) | ((cc_uint32)b << 16) | ((cc_uint32)c << 8) | (cc_uint32)d)
#define WAV_FMT_IMA_ADPCM 0x11

struct AdpcmDecoder {
	struct Stream* source;
	cc_uint32 left; /* Number of bytes of sample data left */
	cc_uint8 block[ADPCM_BLOCK_ALIGN];
};

static cc_result Music_DecodeAdpcm(void* obj, cc_int16* data, int* samples) {
	struct AdpcmDecoder* d = (struct AdpcmDecoder*)obj;
	struct AdpcmState state;
	cc_uint32 i, size = min(d->left, ADPCM_BLOCK_ALIGN);
	int count = 1;
	cc_result res;

	if (size <= 4) return ERR_END_OF_STREAM;
	if ((res = Stream_Read(d->source, d->block, size))) return res;
	d->left -= size;

	/* Each block starts with its first sample and the step index */
	state.predictor = (cc_int16)Mem_ReadU16_LE(d->block);
	state.index     = min(d->block[2], 88);
	data[0]         = state.predictor;

	for (i = 4; i < size; i++)
	{
		data[count++] = Adpcm_DecodeSample(&state, d->block[i] & 0x0F);
		data[count++] = Adpcm_DecodeSample(&state, d->block[i] >> 4);
	}
	*samples = count;
	return 0;
}

/* Plays a mono IMA ADPCM .wav file, as produced when music is fetched */
static cc_result Music_PlayWav(struct Stream* source) {
	cc_uint8 header[16];
	struct AdpcmDecoder* d;
	struct Stream stream;
	cc_uint32 fourCC, size;
	int sampleRate = 0;
	cc_result res;

	/* Memory card reads are slow, so read in large batches */
	d = (struct AdpcmDecoder*)Mem_TryAlloc(1, sizeof(struct AdpcmDecoder) + 8192);
	if (!d) return ERR_OUT_OF_MEMORY;
	Stream_ReadonlyBuffered(&stream, source, d + 1, 8192);
	d->source = &stream;

	if ((res = Stream_Read(&stream, header, 12))) goto cleanup;
	if (Mem_ReadU32_BE(header + 0) != WAV_FourCC('R','I','F','F')) { res = WAV_ERR_STREAM_HDR;  goto cleanup; }
	if (Mem_ReadU32_BE(header + 8) != WAV_FourCC('W','A','V','E')) { res = WAV_ERR_STREAM_TYPE; goto cleanup; }

	for (;;) {
		if ((res = Stream_Read(&stream, header, 8))) goto cleanup;
		fourCC = Mem_ReadU32_BE(header + 0);
		size   = Mem_ReadU32_LE(header + 4);
		if (fourCC == WAV_FourCC('d','a','t','a')) break;

		if (fourCC == WAV_FourCC('f','m','t',' ') && size >= 16) {
			if ((res = Stream_Read(&stream, header, 16))) goto cleanup;
			size -= 16;

			if (Mem_ReadU16_LE(header + 0)  != WAV_FMT_IMA_ADPCM || Mem_ReadU16_LE(header + 2) != 1 ||
				Mem_ReadU16_LE(header + 12) != ADPCM_BLOCK_ALIGN) { res = WAV_ERR_DATA_TYPE; goto cleanup; }
			sampleRate = (int)Mem_ReadU32_LE(header + 4);
		}

		/* Skip over rest of the chunk (chunks are padded to an even size) */
		if ((res = stream.Skip(&stream, size + (size & 1)))) goto cleanup;
	}

	if (!sampleRate) { res = WAV_ERR_DATA_TYPE; goto cleanup; }
	d->left = size;
	res = Music_Stream(1, sampleRate, ADPCM_BLOCK_SAMPLES, Music_DecodeAdpcm, d);

cleanup:
	Mem_Free(d);
	return res == ERR_END_OF_STREAM ? 0 : res;
}
#endif

static void Music_AddFile(const cc_string* path, void* obj, int isDirectory) {
	struct StringsBuffer* files = (struct StringsBuffer*)obj;
	static const cc_string ogg  = String_FromConst(".ogg");
#ifdef CC_BUILD_COMPACTMUSIC
	static const cc_string wav  = String_FromConst(".wav");
#endif

	if (isDirectory) {
		Directory_Enum(path, obj, Music_AddFile);
	} else if (String_CaselessEnds(path, &ogg)) {
		StringsBuffer_Add(files, path);
	}
#ifdef CC_BUILD_COMPACTMUSIC
	else if (String_CaselessEnds(path, &wav)) {
		StringsBuffer_Add(files, path);
	}
#endif
}

static cc_result Music_PlayFile(const cc_string* path, struct Stream* stream) {
#ifdef CC_BUILD_COMPACTMUSIC
	static const cc_string wav = String_FromConst(".wav");
	if (String_CaselessEnds(path, &wav)) return Music_PlayWav(stream);
#endif
	return Music_PlayOgg(stream);
}

#ifdef CC_BUILD_BUNDLED_MUSIC
/* Adds music bundled with the game (e.g. on the disc), named MUSIC1.WAV, MUSIC2.WAV etc */
/*  These play as well as any downloaded music, so there's always some music without network */
static void Music_AddBundled(struct StringsBuffer* files) {
	cc_string path; char pathBuffer[FILENAME_SIZE];
	const char* dir = Platform_BundledMusicDir();
	cc_filepath raw_path;
	int i;
	if (!dir) return;

	for (i = 1; i <= 99; i++)
	{
		String_InitArray(path, pathBuffer);
		String_Format2(&path, "%cMUSIC%i.WAV", dir, &i);

		Platform_EncodePath(&raw_path, &path);
		if (!File_Exists(&raw_path)) break;
		StringsBuffer_Add(files, &path);
	}
}
#endif

static void Music_RunLoop(void) {
	struct StringsBuffer files;
	cc_string path;
	RNGState rnd;
	struct Stream stream;
	cc_filepath raw_path;
	int idx, delay;
	cc_result res = 0;

	StringsBuffer_SetLengthBits(&files, STRINGSBUFFER_DEF_LEN_SHIFT);
	StringsBuffer_Init(&files);
	Directory_Enum(&audio_dir, &files, Music_AddFile);
#ifdef CC_BUILD_BUNDLED_MUSIC
	Music_AddBundled(&files);
#endif

	Random_SeedFromCurrentTime(&rnd);
	res = Audio_Init(&music_ctx, AUDIO_MAX_BUFFERS);
	if (res) music_stopping = true;

	while (!music_stopping && files.count) {
		idx  = Random_Next(&rnd, files.count);
		path = StringsBuffer_UNSAFE_Get(&files, idx);
		Platform_Log1("playing music file: %s", &path);

		Platform_EncodePath(&raw_path, &path);
		res = Stream_OpenPath(&stream, &raw_path);
		if (res) { Logger_IOWarn2(res, "opening", &raw_path); break; }

		res = Music_PlayFile(&path, &stream);
		if (res) { Logger_SimpleWarn2(res, "playing", &path); }

		/* No point logging error for closing readonly file */
		(void)stream.Close(&stream);

		if (music_stopping) break;
		delay = Random_Range(&rnd, music_minDelay, music_maxDelay);
		Waitable_WaitFor(music_waitable, delay);
	}

	if (res) {
		Chat_AddRaw("&cDisabling music");
		Audio_MusicVolume = 0;
	}
	Audio_Close(&music_ctx);
	StringsBuffer_Clear(&files);

	if (music_joining) return;
	Thread_Detach(music_thread);
	music_thread = NULL;
}

static void Music_Start(void) {
	if (music_thread) return;
	if (!AudioBackend_Init()) {
		AudioBackend_Free();
		Audio_MusicVolume = 0;
		return;
	}

	music_joining  = false;
	music_stopping = false;

	Thread_Run(&music_thread, Music_RunLoop, 256 * 1024, "Music");
}

static void Music_Stop(void) {
	music_joining  = true;
	music_stopping = true;
	Waitable_Signal(music_waitable);
	
	if (music_thread) Thread_Join(music_thread);
	music_thread = NULL;
}

static void Music_Init(void) {
	int volume;
	/* music is delayed between 2 - 7 minutes by default */
	music_minDelay = Options_GetInt(OPT_MIN_MUSIC_DELAY, 0, 3600, 120) * MILLIS_PER_SEC;
	music_maxDelay = Options_GetInt(OPT_MAX_MUSIC_DELAY, 0, 3600, 420) * MILLIS_PER_SEC;
	music_waitable = Waitable_Create("Music sleep");

	volume = Options_GetInt(OPT_MUSIC_VOLUME, 0, 100, DEFAULT_MUSIC_VOLUME);
	Audio_SetMusic(volume);
}

static void Music_Free(void) {
	Music_Stop();
	Waitable_Free(music_waitable);
}
#endif


/*########################################################################################################################*
*--------------------------------------------------------General----------------------------------------------------------*
*#########################################################################################################################*/
void Audio_SetSounds(int volume) {
	Audio_SoundsVolume = volume;
	if (volume) Sounds_Start();
	else        Sounds_Stop();
}

void Audio_SetMusic(int volume) {
	Audio_MusicVolume = volume;
	if (volume) Music_Start();
	else        Music_Stop();
}

static void OnInit(void) {
	Utils_EnsureDirectory("audio");

	Sounds_Init();
	Music_Init();
}

static void OnFree(void) {
	Sounds_Free();
	Music_Free();
	AudioBackend_Free();
}

struct IGameComponent Audio_Component = {
	OnInit, /* Init  */
	OnFree  /* Free  */
};
