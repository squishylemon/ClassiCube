#include "World.h"
#include "Logger.h"
#include "String_.h"
#include "Platform.h"
#include "Event.h"
#include "Block.h"
#include "Entity.h"
#include "ExtMath.h"
#include "Physics.h"
#include "Game.h"
#include "TexturePack.h"
#include "Window.h"
#include "Funcs.h"

struct _WorldData World;
static char nameBuffer[STRING_SIZE];
/*########################################################################################################################*
*----------------------------------------------------------World----------------------------------------------------------*
*#########################################################################################################################*/
static void GenerateNewUuid(void) {
	RNGState rnd;
	int i;
	Random_SeedFromCurrentTime(&rnd);

	/* seed a bit more randomness for uuid */
	for (i = 0; i < Game_Username.length; i++) {
		Random_Next(&rnd, Game_Username.buffer[i] + 3);
	}

	for (i = 0; i < WORLD_UUID_LEN; i++) {
		World.Uuid[i] = Random_Next(&rnd, 256);
	}

	/* Set version and variant bits */
	World.Uuid[6] &= 0x0F;
	World.Uuid[6] |= 0x40; /* version 4*/
	World.Uuid[8] &= 0x3F;
	World.Uuid[8] |= 0x80; /* variant 2*/
}

#if defined CC_BUILD_LOWMEM && !defined CC_BUILD_COMPACTMAP
/* Instead of freeing the previous map's blocks, they are kept around and reused for */
/*  the next map. Otherwise, the memory freed by the previous map would quickly get */
/*  split up by other small allocations (e.g. textures), and there would then no longer */
/*  be a large enough contiguous block of memory to allocate the next map's blocks in */
static BlockRaw* spare_blocks;
static cc_uint32 spare_volume;

BlockRaw* World_TryAllocBlocks(cc_uint32 volume) {
	BlockRaw* blocks;
	cc_uint32 spareVolume;

	if (spare_blocks && volume <= spare_volume) {
		blocks       = spare_blocks;
		spare_blocks = NULL;
		spare_volume = 0;
		return blocks;
	}

	blocks = (BlockRaw*)Mem_TryAlloc(volume, 1);
	if (blocks || !spare_blocks) return blocks;

	/* Not enough memory, so try again after freeing the spare blocks */
	spareVolume = spare_volume;
	World_FreeSpareBlocks();
	blocks = (BlockRaw*)Mem_TryAlloc(volume, 1);
	if (blocks) return blocks;

	/* Still not enough memory for this map (e.g. it's too large), so reclaim */
	/*  the spare blocks memory before it gets split up by other allocations */
	spare_blocks = (BlockRaw*)Mem_TryAlloc(spareVolume, 1);
	spare_volume = spare_blocks ? spareVolume : 0;
	return NULL;
}

void World_FreeBlocks(BlockRaw* blocks, cc_uint32 volume) {
	if (!blocks) return;
	/* Only keep the largest blocks array around */
	if (spare_blocks && spare_volume >= volume) { Mem_Free(blocks); return; }

	World_FreeSpareBlocks();
	spare_blocks = blocks;
	spare_volume = volume;
}

void World_FreeSpareBlocks(void) {
	Mem_Free(spare_blocks);
	spare_blocks = NULL;
	spare_volume = 0;
}
#else
BlockRaw* World_TryAllocBlocks(cc_uint32 volume) {
	return (BlockRaw*)Mem_TryAlloc(volume, 1);
}

void World_FreeBlocks(BlockRaw* blocks, cc_uint32 volume) { Mem_Free(blocks); }
void World_FreeSpareBlocks(void) { }
#endif


#ifdef CC_BUILD_COMPACTMAP
/*########################################################################################################################*
*-------------------------------------------------------Compact map-------------------------------------------------------*
*#########################################################################################################################*/
/* Most chunks in a map are entirely one block (e.g. air above the ground, or dirt/stone below it), */
/*  so storing such chunks as a single block greatly reduces memory usage of large maps */
static void Chunks_Free(void) {
	int i;
	if (World.ChunkBlocks) {
		for (i = 0; i < World.ChunksCount; i++) { Mem_Free(World.ChunkBlocks[i]); }
	}

	Mem_Free(World.ChunkBlocks);
	Mem_Free(World.ChunkFills);
	World.ChunkBlocks = NULL;
	World.ChunkFills  = NULL;
}

/* Copies count blocks starting at the given packed index into dst */
typedef void (*FetchBlocksRow)(BlockRaw* dst, cc_uint32 index, int count);
/* Called after all chunks up to (or with topDown, from) the given packed index have been built */
typedef void (*ReleaseBlocks)(cc_uint32 index);

/* topDown builds chunks starting from the top of the map, so that the source blocks can be */
/*  released from the end, while chunks are still being built (see ReleaseFlatRows) */
static cc_bool Chunks_Build(FetchBlocksRow fetch, ReleaseBlocks release, cc_bool topDown) {
	BlockRaw chunk[CHUNK_SIZE_3];
	int cx, cy, cz, x1, y1, z1, xCount, yCount, zCount;
	int y, z, i, j, layer;
	BlockRaw* blocks;
	BlockRaw fill;

	World.ChunkBlocks = (BlockRaw**)Mem_TryAllocCleared(World.ChunksCount, sizeof(BlockRaw*));
	World.ChunkFills  = (BlockRaw*) Mem_TryAllocCleared(World.ChunksCount, sizeof(BlockRaw));
	if (!World.ChunkBlocks || !World.ChunkFills) { Chunks_Free(); return false; }

	for (layer = 0; layer < World.ChunksY; layer++)
	{
		cy = topDown ? World.ChunksY - 1 - layer : layer;
		y1 = cy << CHUNK_SHIFT; yCount = min(CHUNK_SIZE, World.Height - y1);

		for (cz = 0; cz < World.ChunksZ; cz++)
		{
			z1 = cz << CHUNK_SHIFT; zCount = min(CHUNK_SIZE, World.Length - z1);

			for (cx = 0; cx < World.ChunksX; cx++)
			{
				x1 = cx << CHUNK_SHIFT; xCount = min(CHUNK_SIZE, World.Width - x1);

				/* Parts of chunks outside the map are never accessed, so just leave them as fill */
				fetch(&fill, World_Pack(x1, y1, z1), 1);
				Mem_Set(chunk, fill, sizeof(chunk));

				for (y = 0; y < yCount; y++)
					for (z = 0; z < zCount; z++)
				{
					fetch(&chunk[World_ChunkLocal(0, y, z)], World_Pack(x1, y1 + y, z1 + z), xCount);
				}

				i = World_ChunkPack(cx, cy, cz);
				World.ChunkFills[i] = fill;

				for (j = 0; j < CHUNK_SIZE_3; j++)
				{
					if (chunk[j] != fill) break;
				}
				if (j == CHUNK_SIZE_3) continue;

				blocks = (BlockRaw*)Mem_TryAlloc(CHUNK_SIZE_3, 1);
				if (!blocks) { Chunks_Free(); return false; }

				Mem_Copy(blocks, chunk, CHUNK_SIZE_3);
				World.ChunkBlocks[i] = blocks;
			}
		}

		if (release) release(World_Pack(0, topDown ? y1 : y1 + yCount, 0));
	}

	for (i = 0, j = 0; i < World.ChunksCount; i++)
	{
		if (World.ChunkBlocks[i]) j++;
	}
	Platform_Log2("Map chunks: %i of %i not uniform", &j, &World.ChunksCount);
	return true;
}

BlockID World_GetBlockAt(int index) {
	int x, y, z;
	World_Unpack(index, x, y, z);
	return World_GetBlock(x, y, z);
}

void World_SetBlock(int x, int y, int z, BlockID block) {
	int i = World_ChunkPack(x >> CHUNK_SHIFT, y >> CHUNK_SHIFT, z >> CHUNK_SHIFT);
	BlockRaw* blocks = World.ChunkBlocks[i];

	/* Lazily expand chunks that were entirely one block */
	if (!blocks) {
		if (World.ChunkFills[i] == block) return;

		blocks = (BlockRaw*)Mem_TryAlloc(CHUNK_SIZE_3, 1);
		if (!blocks) { Platform_LogConst("Out of memory, unable to change block"); return; }

		Mem_Set(blocks, World.ChunkFills[i], CHUNK_SIZE_3);
		World.ChunkBlocks[i] = blocks;
	}
	blocks[World_ChunkLocal(x, y, z)] = (BlockRaw)block;
}


/*########################################################################################################################*
*-------------------------------------------------------Staged map--------------------------------------------------------*
*#########################################################################################################################*/
/* Servers send the map's dimensions only after all of the map's blocks, so the blocks can't */
/*  be put into chunks as they are received. Instead they are stored in 4096 block 'segments' */
/*  (where segments that are all the same block are stored as a single block, and other */
/*  segments are run length encoded), and then converted into chunks once the dimensions are known */
#define SEGMENT_SHIFT 12
#define SEGMENT_SIZE  (1 << SEGMENT_SHIFT)
#define SEGMENT_MASK  (SEGMENT_SIZE - 1)
#define DECODE_CACHE_SIZE 64

static BlockRaw** staged_segs;
static BlockRaw*  staged_fills;
static cc_uint16* staged_rleLens; /* 0 if segment is stored uncompressed */
static cc_uint32 staged_volume, staged_count, staged_committed, staged_index;
static BlockRaw staged_buffer[SEGMENT_SIZE];
static BlockRaw rle_buffer[SEGMENT_SIZE];

/* Recently decoded segments, so they don't need to be decoded again for each chunk */
static BlockRaw* decode_cache;
static cc_uint32 decode_tags[DECODE_CACHE_SIZE];
static int decode_slots;

void World_FreeStagedMap(void) {
	cc_uint32 i;
	if (staged_segs) {
		for (i = 0; i < staged_count; i++) { Mem_Free(staged_segs[i]); }
	}

	Mem_Free(staged_segs);
	Mem_Free(staged_fills);
	Mem_Free(staged_rleLens);
	Mem_Free(decode_cache);
	staged_segs    = NULL;
	staged_fills   = NULL;
	staged_rleLens = NULL;
	decode_cache   = NULL;
	staged_volume    = 0;
	staged_count     = 0;
	staged_committed = 0;
	staged_index     = 0;
}

cc_bool World_BeginStagedMap(cc_uint32 volume) {
	World_FreeStagedMap();
	staged_count = (volume + SEGMENT_MASK) >> SEGMENT_SHIFT;
	if (!staged_count) return false;

	/* Segments that are never received are treated as air */
	staged_segs    = (BlockRaw**)Mem_TryAllocCleared(staged_count, sizeof(BlockRaw*));
	staged_fills   = (BlockRaw*) Mem_TryAllocCleared(staged_count, sizeof(BlockRaw));
	staged_rleLens = (cc_uint16*)Mem_TryAllocCleared(staged_count, sizeof(cc_uint16));
	if (!staged_segs || !staged_fills || !staged_rleLens) { World_FreeStagedMap(); return false; }

	staged_volume = volume;
	return true;
}

BlockRaw* World_GetStagingBuffer(cc_uint32 index, cc_uint32* count) {
	*count = min(SEGMENT_SIZE - (index & SEGMENT_MASK), staged_volume - index);
	return &staged_buffer[index & SEGMENT_MASK];
}

/* Run length encodes the staging buffer as (run length - 1, block) pairs */
/* Returns 0 if the encoded data would not be smaller than the original data */
static int EncodeSegment(int count) {
	int i = 0, len = 0, run;
	BlockRaw block;

	while (i < count)
	{
		block = staged_buffer[i];
		for (run = 1; i + run < count && run < 256 && staged_buffer[i + run] == block; run++) { }

		if (len + 2 >= count) return 0;
		rle_buffer[len++] = (BlockRaw)(run - 1);
		rle_buffer[len++] = block;
		i += run;
	}
	return len;
}

static cc_bool CommitSegment(cc_uint32 seg, int count) {
	BlockRaw fill = staged_buffer[0];
	BlockRaw* blocks;
	int i, len;

	for (i = 1; i < count; i++)
	{
		if (staged_buffer[i] != fill) break;
	}
	staged_fills[seg] = fill;
	if (i == count) return true;

	len = EncodeSegment(count);
	if (len) {
		blocks = (BlockRaw*)Mem_TryAlloc(len, 1);
		if (!blocks) return false;
		Mem_Copy(blocks, rle_buffer, len);
	} else {
		blocks = (BlockRaw*)Mem_TryAlloc(SEGMENT_SIZE, 1);
		if (!blocks) return false;
		Mem_Copy(blocks, staged_buffer, count);
	}

	staged_segs[seg]    = blocks;
	staged_rleLens[seg] = (cc_uint16)len;
	return true;
}

static BlockRaw* GetDecodedSegment(cc_uint32 seg) {
	BlockRaw* src = staged_segs[seg];
	BlockRaw* dst;
	int i, len, slot;
	if (!staged_rleLens[seg]) return src;

	/* Mix in higher bits, since the segments used by a chunk are often 'map width * length' apart */
	slot = (int)((seg ^ (seg >> 6) ^ (seg >> 12)) % decode_slots);
	dst  = decode_cache + slot * SEGMENT_SIZE;
	if (decode_tags[slot] == seg) return dst;

	len = staged_rleLens[seg];
	for (i = 0; i < len; i += 2)
	{
		Mem_Set(dst, src[i + 1], src[i] + 1);
		dst += src[i] + 1;
	}

	decode_tags[slot] = seg;
	return decode_cache + slot * SEGMENT_SIZE;
}

static cc_bool AllocDecodeCache(void) {
	int i;
	/* Fallback to a smaller cache (slower) when low on memory */
	for (decode_slots = DECODE_CACHE_SIZE; decode_slots >= 1; decode_slots /= 4)
	{
		decode_cache = (BlockRaw*)Mem_TryAlloc(decode_slots, SEGMENT_SIZE);
		if (decode_cache) break;
	}
	if (!decode_cache) return false;

	for (i = 0; i < DECODE_CACHE_SIZE; i++) { decode_tags[i] = (cc_uint32)-1; }
	return true;
}

/* Commits the segment currently being written to, if it hasn't been already */
static cc_bool CommitCurrentSegment(void) {
	if (!staged_index || ((staged_index - 1) >> SEGMENT_SHIFT) != staged_committed) return true;

	staged_committed++;
	return CommitSegment(staged_committed - 1, ((staged_index - 1) & SEGMENT_MASK) + 1);
}

cc_bool World_StagedBlocksWritten(cc_uint32 index) {
	staged_index = index;
	/* Commit when the segment is entirely filled, or when the end of the map is reached */
	if ((index & SEGMENT_MASK) && index < staged_volume) return true;
	return CommitCurrentSegment();
}

static void FetchStagedRow(BlockRaw* dst, cc_uint32 index, int count) {
	cc_uint32 seg;
	int n;

	while (count > 0)
	{
		seg = index >> SEGMENT_SHIFT;
		n   = min(count, SEGMENT_SIZE - (int)(index & SEGMENT_MASK));

		if (staged_segs[seg]) {
			Mem_Copy(dst, &GetDecodedSegment(seg)[index & SEGMENT_MASK], n);
		} else {
			Mem_Set(dst, staged_fills[seg], n);
		}
		dst += n; index += n; count -= n;
	}
}

static void ReleaseStagedRows(cc_uint32 index) {
	cc_uint32 seg;
	/* Free segments that are entirely before the given index, as they will never be needed again */
	for (seg = 0; seg < staged_count && (seg + 1) * SEGMENT_SIZE <= index; seg++)
	{
		Mem_Free(staged_segs[seg]);
		staged_segs[seg] = NULL;
	}
}

static BlockRaw* flat_blocks;
static void FetchFlatRow(BlockRaw* dst, cc_uint32 index, int count) {
	Mem_Copy(dst, &flat_blocks[index], count);
}

/* Shrinks the blocks array to only the rows below the given index, as rows above have already */
/*  been put into chunks. This way, a large map doesn't need memory for both all of its blocks */
/*  and all of its chunks at the same time (e.g. 512x64x512 would need ~29 MB instead of ~17 MB) */
static void ReleaseFlatRows(cc_uint32 index) {
	BlockRaw* blocks;
	if (!index) return;

	/* Shrinking happens in place, so the memory at the end can be reused by chunks */
	blocks = (BlockRaw*)Mem_TryRealloc(flat_blocks, index, 1);
	if (blocks) flat_blocks = blocks;
}
#endif

void World_Reset(void) {
#ifdef EXTENDED_BLOCKS
	if (World.Blocks != World.Blocks2) Mem_Free(World.Blocks2);
	World.Blocks2 = NULL;
	World.IDMask  = 0xFF;
#endif
#ifdef CC_BUILD_COMPACTMAP
	Chunks_Free();
	World_FreeStagedMap();
#endif
	World_FreeBlocks(World.Blocks, World.Volume);
	World.Blocks = NULL;
	String_InitArray(World.Name, nameBuffer);

	World_SetDimensions(0, 0, 0);
	World.Loaded   = false;
	World.LastSave = -200;
	World.Seed     = 0;
	Env_Reset();
}

void World_NewMap(void) {
	World_Reset();
	Event_RaiseVoid(&WorldEvents.NewMap);
}

static void FinishNewMap(int height) {
	World.Name.length = 0;
	if (Env.EdgeHeight == -1)   { Env.EdgeHeight   = height / 2; }
	if (Env.CloudsHeight == -1) { Env.CloudsHeight = height + 2; }

	GenerateNewUuid();
	World.Loaded = true;
	Event_RaiseVoid(&WorldEvents.MapLoaded);
}

#ifdef CC_BUILD_COMPACTMAP
static void MapBuildFailed(int* height) {
	/* Free as much memory as possible first, as showing the dialog needs memory too */
	World_FreeStagedMap();
	Window_ShowDialog("Out of memory", "Not enough free memory to load the map.\nTry joining a different map.");
	*height = 0;
	World_SetDimensions(0, 0, 0);
}

void World_SetNewMap(BlockRaw* blocks, int width, int height, int length) {
	/* TODO: TEMP HACK */
	if (!blocks) { width = 0; height = 0; length = 0; }
	World_SetDimensions(width, height, length);

	flat_blocks = blocks;
	if (World.Volume && !Chunks_Build(FetchFlatRow, ReleaseFlatRows, true)) MapBuildFailed(&height);

	Mem_Free(flat_blocks);
	flat_blocks = NULL;
	World.Blocks = NULL;
	FinishNewMap(height);
}

void World_SetNewStagedMap(int width, int height, int length) {
	World_SetDimensions(width, height, length);

	if (!staged_segs || (cc_uint32)World.Volume != staged_volume) {
		height = 0;
		World_SetDimensions(0, 0, 0);
	} else if (!CommitCurrentSegment() || !AllocDecodeCache() || !Chunks_Build(FetchStagedRow, ReleaseStagedRows, false)) {
		MapBuildFailed(&height);
	}

	World_FreeStagedMap();
	FinishNewMap(height);
}
#else
void World_SetNewMap(BlockRaw* blocks, int width, int height, int length) {
	/* TODO: TEMP HACK */
	if (!blocks) { width = 0; height = 0; length = 0; }

	World_SetDimensions(width, height, length);
	World.Blocks = blocks;

	if (!World.Volume) World.Blocks = NULL;
#ifdef EXTENDED_BLOCKS
	/* .cw maps may have set this to a non-NULL when importing */
	if (!World.Blocks2) {
		World.Blocks2 = World.Blocks;
		World.IDMask  = 0xFF;
	}
#endif
	FinishNewMap(height);
}
#endif

CC_NOINLINE void World_SetDimensions(int width, int height, int length) {
	World.Width  = width; World.Height = height; World.Length = length;
	World.Volume = width * height * length;

	World.OneY = width * length;
	World.MaxX = width  - 1;
	World.MaxY = height - 1;
	World.MaxZ = length - 1;

	World.ChunksX = (width  + CHUNK_MAX) >> CHUNK_SHIFT;
	World.ChunksY = (height + CHUNK_MAX) >> CHUNK_SHIFT;
	World.ChunksZ = (length + CHUNK_MAX) >> CHUNK_SHIFT;

	World.ChunksCount = World.ChunksX * World.ChunksY * World.ChunksZ;
}

#ifdef EXTENDED_BLOCKS
void World_SetMapUpper(BlockRaw* blocks) {
	World.Blocks2 = blocks;
	World.IDMask  = 0x3FF;
}
#endif

void World_OutOfMemory(void) {
	Window_ShowDialog("Out of memory", "Not enough free memory to load the map.\nTry joining a different map.");
	World_Reset();
}


#ifdef EXTENDED_BLOCKS
static CC_NOINLINE void LazyInitUpper(int i, BlockID block) {
	BlockRaw* data = (BlockRaw*)Mem_TryAllocCleared(World.Volume, 1);
	if (!data) { World_OutOfMemory(); return; }

	World_SetMapUpper(data);
	World.Blocks2[i] = (BlockRaw)(block >> 8);
}

void World_SetBlock(int x, int y, int z, BlockID block) {
	int i = World_Pack(x, y, z);
	World.Blocks[i] = (BlockRaw)block;

	/* defer allocation of second map array if possible */
	if (World.Blocks == World.Blocks2) {
		if (block < 256) return;
		LazyInitUpper(i, block);
		return;
	}
	World.Blocks2[i] = (BlockRaw)(block >> 8);
}
#elif !defined CC_BUILD_COMPACTMAP
void World_SetBlock(int x, int y, int z, BlockID block) {
	World.Blocks[World_Pack(x, y, z)] = block;
}
#endif

BlockID World_GetPhysicsBlock(int x, int y, int z) {
	if (y < 0 || !World_ContainsXZ(x, z)) return BLOCK_BEDROCK;
	if (y >= World.Height) return BLOCK_AIR;

	return World_GetBlock(x, y, z);
}

BlockID World_SafeGetBlock(int x, int y, int z) {
	return World_Contains(x, y, z) ? World_GetBlock(x, y, z) : BLOCK_AIR;
}


/*########################################################################################################################*
*-------------------------------------------------------Environment-------------------------------------------------------*
*#########################################################################################################################*/
#define Env_Set(src, dst, var) \
if (src != dst) { dst = src; Event_RaiseInt(&WorldEvents.EnvVarChanged, var); }

struct _EnvData Env;
const char* const Weather_Names[3] = { "Sunny", "Rainy", "Snowy" };

void Env_Reset(void) {
	Env.EdgeHeight   = -1;
	Env.SidesOffset  = -2;
	Env.CloudsHeight = -1;

	Env.EdgeBlock  = BLOCK_STILL_WATER;
	Env.SidesBlock = BLOCK_BEDROCK;

	Env.CloudsSpeed    = 1.0f;
	Env.WeatherSpeed   = 1.0f;
	Env.WeatherFade    = 1.0f;
	Env.SkyboxHorSpeed = 0.0f;
	Env.SkyboxVerSpeed = 0.0f;

	Env.ShadowCol = ENV_DEFAULT_SHADOW_COLOR;
	PackedCol_GetShaded(Env.ShadowCol, &Env.ShadowXSide,
		&Env.ShadowZSide, &Env.ShadowYMin);

	Env.SunCol = ENV_DEFAULT_SUN_COLOR;
	PackedCol_GetShaded(Env.SunCol, &Env.SunXSide,
		&Env.SunZSide, &Env.SunYMin);

	Env.SkyCol       = ENV_DEFAULT_SKY_COLOR;
	Env.FogCol       = ENV_DEFAULT_FOG_COLOR;
	Env.CloudsCol    = ENV_DEFAULT_CLOUDS_COLOR;
	Env.SkyboxCol    = ENV_DEFAULT_SKYBOX_COLOR;
	Env.LavaLightCol = ENV_DEFAULT_LAVALIGHT_COLOR;
	Env.LampLightCol = ENV_DEFAULT_LAMPLIGHT_COLOR;
	Env.Weather   = WEATHER_SUNNY;
	Env.ExpFog    = false;
}


void Env_SetEdgeBlock(BlockID block) {
	/* some server software wrongly uses this value */
	if (block == 255 && !Block_IsCustomDefined(255)) block = BLOCK_STILL_WATER; 
	Env_Set(block, Env.EdgeBlock, ENV_VAR_EDGE_BLOCK);
}
void Env_SetSidesBlock(BlockID block) {
	/* some server software wrongly uses this value */
	if (block == 255 && !Block_IsCustomDefined(255)) block = BLOCK_BEDROCK;
	Env_Set(block, Env.SidesBlock, ENV_VAR_SIDES_BLOCK);
}

void Env_SetEdgeHeight(int height) {
	Env_Set(height, Env.EdgeHeight, ENV_VAR_EDGE_HEIGHT);
}
void Env_SetSidesOffset(int offset) {
	Env_Set(offset, Env.SidesOffset, ENV_VAR_SIDES_OFFSET);
}
void Env_SetCloudsHeight(int height) {
	Env_Set(height, Env.CloudsHeight, ENV_VAR_CLOUDS_HEIGHT);
}
void Env_SetCloudsSpeed(float speed) {
	Env_Set(speed, Env.CloudsSpeed, ENV_VAR_CLOUDS_SPEED);
}

void Env_SetWeatherSpeed(float speed) {
	Env_Set(speed, Env.WeatherSpeed, ENV_VAR_WEATHER_SPEED);
}
void Env_SetWeatherFade(float rate) {
	Env_Set(rate, Env.WeatherFade, ENV_VAR_WEATHER_FADE);
}
void Env_SetWeather(int weather) {
	Env_Set(weather, Env.Weather, ENV_VAR_WEATHER);
}
void Env_SetExpFog(cc_bool expFog) {
	Env_Set(expFog, Env.ExpFog, ENV_VAR_EXP_FOG);
}
void Env_SetSkyboxHorSpeed(float speed) {
	Env_Set(speed, Env.SkyboxHorSpeed, ENV_VAR_SKYBOX_HOR_SPEED);
}
void Env_SetSkyboxVerSpeed(float speed) {
	Env_Set(speed, Env.SkyboxVerSpeed, ENV_VAR_SKYBOX_VER_SPEED);
}

void Env_SetSkyCol(PackedCol color) {
	Env_Set(color, Env.SkyCol, ENV_VAR_SKY_COLOR);
}
void Env_SetFogCol(PackedCol color) {
	Env_Set(color, Env.FogCol, ENV_VAR_FOG_COLOR);
}
void Env_SetCloudsCol(PackedCol color) {
	Env_Set(color, Env.CloudsCol, ENV_VAR_CLOUDS_COLOR);
}
void Env_SetSkyboxCol(PackedCol color) {
	Env_Set(color, Env.SkyboxCol, ENV_VAR_SKYBOX_COLOR);
}
void Env_SetLavaLightCol(PackedCol color) {
	Env_Set(color, Env.LavaLightCol, ENV_VAR_LAVALIGHT_COLOR);
}
void Env_SetLampLightCol(PackedCol color) {
	Env_Set(color, Env.LampLightCol, ENV_VAR_LAMPLIGHT_COLOR);
}
void Env_SetSunCol(PackedCol color) {
	PackedCol_GetShaded(color, &Env.SunXSide, &Env.SunZSide, &Env.SunYMin);
	Env_Set(color, Env.SunCol, ENV_VAR_SUN_COLOR);
}
void Env_SetShadowCol(PackedCol color) {
	PackedCol_GetShaded(color, &Env.ShadowXSide, &Env.ShadowZSide, &Env.ShadowYMin);
	Env_Set(color, Env.ShadowCol, ENV_VAR_SHADOW_COLOR);
}


/*########################################################################################################################*
*-------------------------------------------------------Respawning--------------------------------------------------------*
*#########################################################################################################################*/
float Respawn_HighestSolidY(struct AABB* bb) {
	int minX = Math_Floor(bb->Min.x), maxX = Math_Floor(bb->Max.x);
	int minY = Math_Floor(bb->Min.y), maxY = Math_Floor(bb->Max.y);
	int minZ = Math_Floor(bb->Min.z), maxZ = Math_Floor(bb->Max.z);
	float highestY = RESPAWN_NOT_FOUND;

	BlockID block;
	struct AABB blockBB;
	Vec3 v;
	int x, y, z;	

	for (y = minY; y <= maxY; y++) { v.y = (float)y;
		for (z = minZ; z <= maxZ; z++) { v.z = (float)z;
			for (x = minX; x <= maxX; x++) { v.x = (float)x;

				/* TODO: Maybe use how picking gets blocks, so the bedrock */
				/* just below and just on borders of the map is treated as such */
				/* Not sure if this is really necessary though, it seems to work */
				/* just fine already when you're standing on the bottom of the map. */
				block = World_SafeGetBlock(x, y, z);
				Vec3_Add(&blockBB.Min, &v, &Blocks.MinBB[block]);
				Vec3_Add(&blockBB.Max, &v, &Blocks.MaxBB[block]);

				if (Blocks.Collide[block] != COLLIDE_SOLID) continue;
				if (!AABB_Intersects(bb, &blockBB)) continue;
				if (blockBB.Max.y > highestY) highestY = blockBB.Max.y;
			}
		}
	}
	return highestY;
}

Vec3 Respawn_FindSpawnPosition(float x, float z, Vec3 modelSize) {
	Vec3 spawn;
	struct AABB bb;
	float highestY;
	int y;

	Vec3_Set(spawn, x, World.Height + ENTITY_ADJUSTMENT, z);
	AABB_Make(&bb, &spawn, &modelSize);
	spawn.y = 0.0f;
	
	for (y = World.Height; y >= 0; y--) {
		highestY = Respawn_HighestSolidY(&bb);
		if (highestY != RESPAWN_NOT_FOUND) {
			spawn.y = highestY; break;
		}
		bb.Min.y -= 1.0f; bb.Max.y -= 1.0f;
	}
	return spawn;
}

struct IGameComponent World_Component = {
	World_Reset, /* Init  */
	World_Reset  /* Free  */
};
