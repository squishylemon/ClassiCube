#define CC_SCRATCH_VBS_ARE_DYNAMIC
#include "../_GraphicsBase.h"
#include "../Errors.h"
#include "../Logger.h"
#include "../Window.h"

#include <libdragon.h>
#include <malloc.h>
#include <rspq_profile.h>
#include "gpu.i"

/*########################################################################################################################*
*---------------------------------------------------------General---------------------------------------------------------*
*#########################################################################################################################*/
static surface_t zbuffer;
static GfxResourceID white_square;

void Gfx_Create(void) {
	rspq_init();
	//rspq_profile_start();
    rdpq_init();
	//rdpq_debug_start(); // TODO debug
	//rdpq_debug_log(true);

	rdpq_set_mode_standard();
	__rdpq_mode_change_som(SOM_TEXTURE_PERSP, SOM_TEXTURE_PERSP);
	__rdpq_mode_change_som(SOM_ZMODE_MASK,    SOM_ZMODE_OPAQUE);
	rdpq_mode_dithering(DITHER_SQUARE_SQUARE);

    gpu_init();

	// Set alpha compare threshold
	gpu_push_rdp(RDP_CMD_SYNC_PIPE, 0);
	gpu_push_rdp(RDP_CMD_SET_BLEND_COLOR, (0 << 24) | (0 << 16) | (0 << 8) | 127);

    zbuffer = surface_alloc(FMT_RGBA16, display_get_width(), display_get_height());
    
	Gfx.MaxTexWidth  = 512;
	Gfx.MaxTexHeight = 512;
	Gfx.Created      = true;
	
	// TMEM only has 4 KB in it, which can be interpreted as
	// - 1024 32bpp pixels
	// - 2048 16bpp pixels
	// Larger textures are drawn a part at a time (see DrawLargeQuad), which is a lot slower,
	//  so terrain atlases are kept small enough to fit in TMEM
	Gfx.MaxAtlasSize = 1024;

	Gfx.NonPowTwoTexturesSupport = GFX_NONPOW2_FULL;

	Gfx_SetFaceCulling(false);
}

cc_bool Gfx_TryRestoreContext(void) {
	return true;
}

void Gfx_Free(void) {
	Gfx_FreeState();
	gpu_close();
}


/*########################################################################################################################*
*-----------------------------------------------------------Misc----------------------------------------------------------*
*#########################################################################################################################*/
static color_t gfx_clearColor;

cc_result Gfx_TakeScreenshot(struct Stream* output) {
	return ERR_NOT_SUPPORTED;
}

void Gfx_GetApiInfo(cc_string* info) {
	String_AppendConst(info, "-- Using Nintendo 64 --\n");
	String_AppendConst(info, "GPU: Nintendo 64 RDP (LibDragon OpenGL)\n");
	String_AppendConst(info, "T&L: Nintendo 64 RSP (LibDragon OpenGL)\n");
	PrintMaxTextureInfo(info);
}

void Gfx_SetVSync(cc_bool vsync) {
	gfx_vsync = vsync; // TODO update vsync
}

void Gfx_OnWindowResize(int width, int height) {
	Gfx_SetViewport(0, 0, width, height);
}

void Gfx_SetViewport(int x, int y, int w, int h) {
	gpuViewport(x, y, w, h);
}

void Gfx_SetScissor(int x, int y, int w, int h) {
	rdpq_set_scissor(x, y, x + w, y + h);
}

void Gfx_BeginFrame(void) {
	surface_t* disp = display_get();
    rdpq_attach(disp, &zbuffer);
    
	//Platform_LogConst("== BEGIN frame");
}

extern void __rdpq_autosync_change(int mode);
static color_t gfx_clearColor;

void Gfx_ClearBuffers(GfxBuffers buffers) {
	__rdpq_autosync_change(AUTOSYNC_PIPE);
	
	if (buffers & GFX_BUFFER_COLOR)
		rdpq_clear(gfx_clearColor);
	if (buffers & GFX_BUFFER_DEPTH)
		rdpq_clear_z(0xFFFC);
} 

void Gfx_ClearColor(PackedCol color) {
	gfx_clearColor.r = PackedCol_R(color);
	gfx_clearColor.g = PackedCol_G(color);
	gfx_clearColor.b = PackedCol_B(color);
	gfx_clearColor.a = PackedCol_A(color);
	//memcpy(&gfx_clearColor, &color, sizeof(color_t)); // TODO maybe use proper conversion from graphics.h ??
}

void Gfx_EndFrame(void) {
    rdpq_detach_show();

	//rspq_profile_dump();
	//rspq_profile_next_frame();
}


/*########################################################################################################################*
*---------------------------------------------------------Palettes--------------------------------------------------------*
*#########################################################################################################################*/
#define MAX_PAL_4BPP_ENTRIES 16

static CC_INLINE int FindInPalette(BitmapCol* palette, int pal_count, BitmapCol color) {
	for (int i = 0; i < pal_count; i++) 
	{
		if (palette[i] == color) return i;
	}
	return -1;
}

static int CalcPalette(BitmapCol* palette, struct Bitmap* bmp, int rowWidth) {
	int width = bmp->width, height = bmp->height;

	BitmapCol* row = bmp->scan0;
	int pal_count  = 0;
	
	for (int y = 0; y < height; y++, row += rowWidth)
	{
		for (int x = 0; x < width; x++) 
		{
			BitmapCol color = row[x];
			int idx = FindInPalette(palette, pal_count, color);
			if (idx >= 0) continue;

			// Too many distinct colours
			if (pal_count >= MAX_PAL_4BPP_ENTRIES) return 0;

			palette[pal_count] = color;
			pal_count++;
		}
	}
	return pal_count;
}


/*########################################################################################################################*
*---------------------------------------------------------Textures--------------------------------------------------------*
*#########################################################################################################################*/
typedef struct CCTexture {
	surface_t surface;
	rspq_block_t* upload_block; // NULL for textures too large to fit in TMEM
	cc_uint16 palette[MAX_PAL_4BPP_ENTRIES];
} CCTexture;

// Texture that's too large to fit in TMEM (e.g. gui.png), which is drawn a piece at a time
static CCTexture* gfx_largeTex;

void Gfx_BindTexture(GfxResourceID texId) {
	if (!texId) texId = white_square;
	CCTexture* tex = (CCTexture*)texId;

	if (tex->upload_block) {
		gfx_largeTex = NULL;
		rspq_block_run(tex->upload_block);
	} else {
		gfx_largeTex = tex;
		rdpq_mode_tlut(TLUT_NONE);
	}
	gpuSetTexSize(tex->surface.width, tex->surface.height);
}

// A8 B8 G8 R8 > A1 B5 G5 B5
#define To16BitPixel(src) \
	((src & 0x80) >> 7) | ((src & 0xF800) >> 10) | ((src & 0xF80000) >> 13) | ((src & 0xF8000000) >> 16);

static void UploadTexture(CCTexture* tex, int palCount, rdpq_texparms_t* params) {
	rspq_block_begin();

	int fmt = tex->surface.flags & SURFACE_FLAGS_TEXFORMAT;
	if (fmt == FMT_CI4) {
		rdpq_tex_upload_tlut(tex->palette, 0, palCount);
		rdpq_mode_tlut(TLUT_RGBA16);
	} else {
		rdpq_mode_tlut(TLUT_NONE);
	}

	rdpq_tex_upload(TILE0, &tex->surface, params);

	tex->upload_block = rspq_block_end();
}

static CC_INLINE int CalcTextureFormat(cc_bool bit16, int pal_count) {
	if (bit16 && pal_count) return FMT_CI4;

	return bit16 ? FMT_RGBA16 : FMT_RGBA32;
}

// rows are actually 8 byte aligned in TMEM https://github.com/DragonMinded/libdragon/blob/f360fa1bb1fb3ff3d98f4ab58692d40c828636c9/src/rdpq/rdpq_tex.c#L132
// so even though width * height * pixel size may fit within 4096 bytes, after adjusting for 8 byte alignment, row pitch * height may exceed 4096 bytes
// 32bpp textures are split into two halves of TMEM (red/green and blue/alpha), each with 16 bits per pixel
static cc_bool FitsInTMEM(int width, int height, int bpp) {
	if (bpp == 4) return CC_ALIGNUP(width * 2, 8) * height <= 2048;
	return CC_ALIGNUP(width * bpp, 8) * height <= 4096;
}

static cc_bool HasTranslucentPixels(struct Bitmap* bmp, int rowWidth) {
	for (int y = 0; y < bmp->height; y++)
	{
		cc_uint32* row = (cc_uint32*)bmp->scan0 + y * rowWidth;
		for (int x = 0; x < bmp->width; x++)
		{
			cc_uint8 a = row[x] & 0xFF;
			if (a != 0x00 && a != 0xFF) return true;
		}
	}
	return false;
}

GfxResourceID Gfx_AllocTexture(struct Bitmap* bmp, int rowWidth, cc_uint8 flags, cc_bool mipmaps) {
	cc_bool lowres = flags & TEXTURE_FLAG_LOWRES;
	// Without the Expansion Pak there's only 4 MB of RAM, so textures are stored as 16 bpp
	//  to halve how much memory they use (unless they need more than 1 bit of alpha)
	cc_bool bit16  = lowres || (!is_memory_expanded() && !HasTranslucentPixels(bmp, rowWidth));
	cc_bool large  = !FitsInTMEM(bmp->width, bmp->height, bit16 ? 2 : 4);

	CCTexture* tex = Mem_TryAllocCleared(1, sizeof(CCTexture));
	if (!tex) return NULL;

	BitmapCol palette[MAX_PAL_4BPP_ENTRIES];
	int pal_count = 0;
	if (lowres && !large && !(flags & TEXTURE_FLAG_DYNAMIC))
		pal_count = CalcPalette(palette, bmp, rowWidth);

	tex->surface  = surface_alloc(CalcTextureFormat(bit16, pal_count), bmp->width, bmp->height);
	surface_t* fb = &tex->surface;
	if (!fb->buffer) { Mem_Free(tex); return NULL; }

	cc_uint32* src = (cc_uint32*)bmp->scan0;
	cc_uint8*  dst = (cc_uint8*)fb->buffer;
	int width  = bmp->width;
	int height = bmp->height;
		
	if (bit16 && pal_count) {
		for (int i = 0; i < pal_count; i++)
		{
			tex->palette[i] = To16BitPixel(palette[i]);
		}
		
		// 16 bpp requires reducing A8R8G8B8 to A1R5G5B5
		for (int y = 0; y < height; y++) 
		{	
			cc_uint32* src_row = src + y * rowWidth;
			cc_uint8*  dst_row = dst + y * fb->stride;
			
			for (int x = 0; x < (width & ~0x01); x += 2) 
			{
				int pal_0 = FindInPalette(palette, pal_count, *src_row++);
				int pal_1 = FindInPalette(palette, pal_count, *src_row++);
				*dst_row++ = (pal_0 << 4) | pal_1;
			}
			
			// fixup for odd width textures
			if (width & 0x01) {
				int pal_0 = FindInPalette(palette, pal_count, *src_row++);
				*dst_row++ = pal_0 << 4;
			}
		}
	} else if (bit16) {
		// 16 bpp requires reducing A8R8G8B8 to A1R5G5B5
		for (int y = 0; y < height; y++) 
		{	
			cc_uint32* src_row = src + y * rowWidth;
			cc_uint16* dst_row = (cc_uint16*)(dst + y * fb->stride);
			
			for (int x = 0; x < width; x++) 
			{
				dst_row[x] = To16BitPixel(src_row[x]);
			}
		}
	} else {
		// 32 bpp can just be copied straight across
		CopyPixels(dst, fb->stride, 
				   src, rowWidth * BITMAPCOLOR_SIZE,
				   width, height);
	}
	
	rdpq_texparms_t params =
	{
        .s.repeats = (flags & TEXTURE_FLAG_NONPOW2) ? 1 : REPEAT_INFINITE,
        .t.repeats = (flags & TEXTURE_FLAG_NONPOW2) ? 1 : REPEAT_INFINITE,
    };
	// Large textures are instead uploaded a part at a time when drawn (see DrawLargeQuad)
	if (!large) UploadTexture(tex, pal_count, &params);
	return tex;
}

void Gfx_UpdateTexture(GfxResourceID texId, int x, int y, struct Bitmap* part, int rowWidth, cc_bool mipmaps) {
	CCTexture* tex = (CCTexture*)texId;
	surface_t* fb  = &tex->surface;

	cc_uint32* src = (cc_uint32*)part->scan0 + x;

	if (surface_get_format(fb) == FMT_RGBA16) {
		cc_uint8* dst = (cc_uint8*)fb->buffer + (x * 2) + (y * fb->stride);

		for (int srcY = 0; srcY < part->height; srcY++)
		{
			cc_uint32* src_row = src + srcY * rowWidth;
			cc_uint16* dst_row = (cc_uint16*)(dst + srcY * fb->stride);

			for (int srcX = 0; srcX < part->width; srcX++)
			{
				dst_row[srcX] = To16BitPixel(src_row[srcX]);
			}
		}
		return;
	}
	cc_uint8* dst = (cc_uint8*)fb->buffer + (x * 4) + (y * fb->stride);

	for (int srcY = 0; srcY < part->height; srcY++)
	{
		Mem_Copy(dst + srcY * fb->stride,
				 src + srcY * rowWidth,
				 part->width * 4);
	}
}

void Gfx_DeleteTexture(GfxResourceID* texId) {
	CCTexture* tex = (CCTexture*)(*texId);
	if (!tex) return;
	
	if (tex == gfx_largeTex) gfx_largeTex = NULL;
	if (tex->upload_block) rdpq_call_deferred((void (*)(void*))rspq_block_free, tex->upload_block);
	// RDP might not have finished reading from the texture yet
	rdpq_call_deferred(free_uncached, tex->surface.buffer);

	Mem_Free(tex);
	*texId = NULL;
}

static cc_bool bilinear_mode;
static void SetFilterMode(cc_bool bilinear) {
	if (bilinear_mode == bilinear) return;
	bilinear_mode = bilinear;

	uint64_t mode = bilinear ? FILTER_BILINEAR : FILTER_POINT;
	__rdpq_mode_change_som(SOM_SAMPLE_MASK, mode << SOM_SAMPLE_SHIFT);

	int offset = bilinear ? -((1 << TEX_SHIFT) / 2) : 0;
	gpuSetTexOffset(offset, offset);
}

void Gfx_EnableMipmaps(void) {
	// TODO move back to texture instead?
	SetFilterMode(Gfx.Mipmaps);
}

void Gfx_DisableMipmaps(void) {
	SetFilterMode(FILTER_POINT);
}


/*########################################################################################################################*
*-----------------------------------------------------State management----------------------------------------------------*
*#########################################################################################################################*/
void Gfx_SetFaceCulling(cc_bool enabled) { 
	gpuSetCullFace(enabled);
}

static void SetAlphaBlend(cc_bool enabled) { 
	rdpq_mode_blender(enabled ? RDPQ_BLENDER_MULTIPLY : 0);
	__rdpq_mode_change_som(SOM_ZMODE_MASK, enabled ? SOM_ZMODE_TRANSPARENT : SOM_ZMODE_OPAQUE);
}

void Gfx_SetAlphaArgBlend(cc_bool enabled) { }

static void SetAlphaTest(cc_bool enabled) {
	__rdpq_mode_change_som(SOM_ALPHACOMPARE_MASK, enabled ? SOM_ALPHACOMPARE_THRESHOLD : 0);
}

static void SetColorWrite(cc_bool r, cc_bool g, cc_bool b, cc_bool a) {
	//gpuColorMask(r, g, b, a); TODO
}

#define FLAG_Z_WRITE 0x02
static void SetDepthWrite(cc_bool enabled) { 
	__rdpq_mode_change_som(SOM_Z_WRITE, enabled ? SOM_Z_WRITE : 0);

	gpu_attr_z &= ~FLAG_Z_WRITE;
	gpu_attr_z |= enabled ? FLAG_Z_WRITE : 0;
	gpuUpdateFormat();
}

#define FLAG_Z_READ 0x01
static void SetDepthTest(cc_bool enabled) { 
	__rdpq_mode_change_som(SOM_Z_COMPARE, enabled ? SOM_Z_COMPARE : 0);

	gpu_attr_z &= ~FLAG_Z_READ;
	gpu_attr_z |= enabled ? FLAG_Z_READ : 0;
	gpuUpdateFormat();
}

static void Gfx_FreeState(void) { FreeDefaultResources(); }
static void Gfx_RestoreState(void) {
	InitDefaultResources();
	gfx_format = -1;
	
	// 1x1 dummy white texture
	struct Bitmap bmp;
	BitmapCol pixels[1] = { BITMAPCOLOR_WHITE };
	Bitmap_Init(bmp, 1, 1, pixels);
	white_square = Gfx_CreateTexture(&bmp, 0, false);
}

cc_bool Gfx_WarnIfNecessary(void) { return false; }
cc_bool Gfx_GetUIOptions(struct MenuOptionsScreen* s) { return false; }


/*########################################################################################################################*
*---------------------------------------------------------Matrices--------------------------------------------------------*
*#########################################################################################################################*/
void Gfx_CalcOrthoMatrix(struct Matrix* matrix, float width, float height, float zNear, float zFar) {
	/* Transposed, source https://learn.microsoft.com/en-us/windows/win32/opengl/glortho */
	/*   The simplified calculation below uses: L = 0, R = width, T = 0, B = height */
	*matrix = Matrix_Identity;

	matrix->row1.x =  2.0f / width;
	matrix->row2.y = -2.0f / height;
	matrix->row3.z = -2.0f / (zFar - zNear);

	matrix->row4.x = -1.0f;
	matrix->row4.y =  1.0f;
	matrix->row4.z = -(zFar + zNear) / (zFar - zNear);
}

static float Cotangent(float x) { return Math_CosF(x) / Math_SinF(x); }
void Gfx_CalcPerspectiveMatrix(struct Matrix* matrix, float fov, float aspect, float zFar) {
	float zNear = 0.1f;
	float c = Cotangent(0.5f * fov);

	/* Transposed, source https://learn.microsoft.com/en-us/windows/win32/opengl/glfrustum */
	/* For a FOV based perspective matrix, left/right/top/bottom are calculated as: */
	/*   left = -c * aspect, right = c * aspect, bottom = -c, top = c */
	/* Calculations are simplified because of left/right and top/bottom symmetry */
	*matrix = Matrix_Identity;

	matrix->row1.x =  c / aspect;
	matrix->row2.y =  c;
	matrix->row3.z = -(zFar + zNear) / (zFar - zNear);
	matrix->row3.w = -1.0f;
	matrix->row4.z = -(2.0f * zFar * zNear) / (zFar - zNear);
	matrix->row4.w =  0.0f;
}


/*########################################################################################################################*
*----------------------------------------------------------Buffers--------------------------------------------------------*
*#########################################################################################################################*/
#define MAX_CACHED_CALLS 8

struct VertexBufferCache {
	rspq_block_t* blocks[MAX_CACHED_CALLS];
	int offset[MAX_CACHED_CALLS];
	int counts[MAX_CACHED_CALLS];
	// RSP vertices are smaller than textured vertices, so static textured vertex buffers are shrunk
	//  once their vertices have been converted (see Gfx_UnlockVb), and locking them again
	//  afterwards instead uses a temporary buffer (which is then converted into the vertex buffer)
	cc_bool dynamic, shrunk;
	// Whether the RSP might still read this static vertex buffer's vertices (see DrawQuads)
	cc_bool drawn;
	int capacity;
	void* lockBuffer;
} __attribute__((aligned(16)));
struct VertexBuffer {
	struct VertexBufferCache cache;
	char vertices[];
};

static struct VertexBuffer* gfx_vb;
static int vb_count, vb_fmt;

static void VB_ClearCache(struct VertexBuffer* vb) {
	for (int i = 0; i < MAX_CACHED_CALLS; i++)
	{
		rspq_block_t* block = vb->cache.blocks[i];
		if (!block) continue;

		// TODO is this really safe ??
		rdpq_call_deferred((void (*)(void*))rspq_block_free, block);
	}
	Mem_Set(vb->cache.blocks, 0, sizeof(vb->cache.blocks));
	Mem_Set(vb->cache.offset, 0, sizeof(vb->cache.offset));
	Mem_Set(vb->cache.counts, 0, sizeof(vb->cache.counts));
}

static rspq_block_t* VB_GetCached(struct VertexBuffer* vb, int offset, int count) {
	if (!count) return NULL;

	// Lookup previously cached draw commands
	for (int i = 0; i < MAX_CACHED_CALLS; i++)
	{
		if (offset == vb->cache.offset[i] && count == vb->cache.counts[i])
			return vb->cache.blocks[i];
	}

	// Add a new cached draw command
	for (int i = 0; i < MAX_CACHED_CALLS; i++)
	{
		if (vb->cache.blocks[i]) continue;

		rspq_block_begin();
		gpu_pointer = gfx_vb->vertices;
		gpuDrawArrays(offset, count);
		rspq_block_t* block = rspq_block_end();

		vb->cache.blocks[i] = block;
		vb->cache.offset[i] = offset;
		vb->cache.counts[i] = count;
		return block;
	}

	// .. nope, no room for it
	return NULL;
}


GfxResourceID Gfx_CreateIb2(int count, Gfx_FillIBFunc fillFunc, void* obj) {
	return (void*)1;
}

void Gfx_BindIb(GfxResourceID ib)    { }
void Gfx_DeleteIb(GfxResourceID* ib) { }


static GfxResourceID Gfx_AllocStaticVb(VertexFormat fmt, int count) {
	struct VertexBuffer* vb = memalign(16, sizeof(struct VertexBufferCache) + count * strideSizes[fmt]);
	if (!vb) return NULL;

	Mem_Set(&vb->cache, 0, sizeof(vb->cache));
	vb->cache.capacity = count;
	return vb;
}

void Gfx_BindVb(GfxResourceID vb) { 
	gfx_vb = vb; 
}

void Gfx_DeleteVb(GfxResourceID* vb) {
	GfxResourceID data = *vb;
	if (!data) return;

	VB_ClearCache(data);
	rspq_wait(); // for deferred deletes TODO needed?
	Mem_Free(data);
	*vb = NULL;
}

void* Gfx_LockVb(GfxResourceID vb_, VertexFormat fmt, int count) {
	struct VertexBuffer* vb = (struct VertexBuffer*)vb_;
	vb_count = min(count, vb->cache.capacity);
	vb_fmt   = fmt;
	// RSP might still need to draw the current vertices
	if (vb->cache.drawn) { rspq_wait(); vb->cache.drawn = false; }
	if (!vb->cache.shrunk) return vb->vertices;

	vb->cache.lockBuffer = Mem_Alloc(count, strideSizes[fmt], "locking vertex buffer");
	return vb->cache.lockBuffer;
}

void Gfx_UnlockVb(GfxResourceID vb_) {
	struct VertexBuffer* vb = (struct VertexBuffer*)vb_;
	VB_ClearCache(vb); // data may have changed

	void* ptr = vb->cache.lockBuffer ? vb->cache.lockBuffer : vb->vertices;
	if (vb_fmt == VERTEX_FORMAT_COLOURED) {
		convert_coloured_vertices(ptr, vb_count);
	} else {
		convert_textured_vertices(ptr, vb_count);
	}

	if (vb->cache.lockBuffer) {
		Mem_Copy(vb->vertices, ptr, vb_count * sizeof(struct rsp_vertex));
		Mem_Free(vb->cache.lockBuffer);
		vb->cache.lockBuffer = NULL;
	}
	CPU_FlushDataCache(vb->vertices, vb_count * sizeof(struct rsp_vertex));

	if (vb->cache.dynamic || vb->cache.shrunk || vb_fmt != VERTEX_FORMAT_TEXTURED) return;
	vb->cache.shrunk   = true;
	vb->cache.capacity = vb_count;

	// NOTE: newlib's realloc always shrinks memory in place (i.e. it stays at the same address)
	uintptr_t addr = (uintptr_t)vb;
	void* shrunk   = realloc(vb, sizeof(struct VertexBufferCache) + vb_count * sizeof(struct rsp_vertex));
	if ((uintptr_t)shrunk != addr) Process_Abort("Vertex buffer moved when shrinking");
}

static GfxResourceID Gfx_AllocDynamicVb(VertexFormat fmt, int maxVertices) {
	struct VertexBuffer* vb = Gfx_AllocStaticVb(fmt, maxVertices);
	if (vb) vb->cache.dynamic = true;
	return vb;
}

void Gfx_BindDynamicVb(GfxResourceID vb) { Gfx_BindVb(vb); }

void* Gfx_LockDynamicVb(GfxResourceID vb, VertexFormat fmt, int count) {
	return Gfx_LockVb(vb, fmt, count);
}

void Gfx_UnlockDynamicVb(GfxResourceID vb)  { Gfx_UnlockVb(vb); Gfx_BindVb(vb); }

void Gfx_DeleteDynamicVb(GfxResourceID* vb) { Gfx_DeleteVb(vb); }


/*########################################################################################################################*
*-----------------------------------------------------State management----------------------------------------------------*
*#########################################################################################################################*/
static cc_bool depthOnlyRendering;

static void UpdateCombiner(void);

// Fog is linear from 0 to end, calculated per vertex by the RSP and stored in vertex alpha,
//  which the RDP then uses to blend between the pixel colour and the fog colour
static void UpdateFog(void) {
	float end = gfx_fogEnd;
	// Approximate exponential fog with linear fog (see algorithm in EnvRenderer.c)
	if (gfx_fogMode == FOG_EXP) end = -2.99573227355399f / -gfx_fogDensity;

	gpuSetFog(gfx_fogEnabled, end > 0.0f ? 255.0f / end : 0.0f);
}

void Gfx_SetFog(cc_bool enabled) {
	gfx_fogEnabled = enabled;
	rdpq_mode_fog(enabled ? RDPQ_FOG_STANDARD : 0);
	UpdateCombiner();
	UpdateFog();
}

static void SetFogColor(PackedCol color) {
	rdpq_set_fog_color(RGBA32(PackedCol_R(color), PackedCol_G(color), PackedCol_B(color), 0xFF));
}

static void SetFogDensity(float value) { UpdateFog(); }
static void SetFogEnd(float value)     { UpdateFog(); }
static void SetFogMode(FogFunc func)   { UpdateFog(); }

void Gfx_DepthOnlyRendering(cc_bool depthOnly) {
	depthOnlyRendering = depthOnly; // TODO: Better approach? maybe using glBlendFunc instead?
	cc_bool enabled    = !depthOnly;

	//DefaultDepthOnlyRendering(depthOnly);
	gpu_attr_tex = enabled;
	gpuUpdateFormat();
}


/*########################################################################################################################*
*---------------------------------------------------------Matrices--------------------------------------------------------*
*#########################################################################################################################*/
static struct Matrix _view, _proj;

void Gfx_LoadMatrix(MatrixType type, const struct Matrix* matrix) {
	if (type == MATRIX_VIEW) _view = *matrix;
	if (type == MATRIX_PROJ) _proj = *matrix;

	struct Matrix mvp CC_ALIGNED(64);	
	Matrix_Mul(&mvp, &_view, &_proj);
	gpuLoadMatrix((const float*)&mvp);
}

void Gfx_LoadMVP(const struct Matrix* view, const struct Matrix* proj, struct Matrix* mvp) {
	_proj = *proj;
	_view = *view;

	Matrix_Mul(mvp, view, proj);
	gpuLoadMatrix((const float*)mvp);
}

void Gfx_EnableTextureOffset(float x, float y) {
	// TODO
}

void Gfx_DisableTextureOffset(void) { }


/*########################################################################################################################*
*--------------------------------------------------------Rendering--------------------------------------------------------*
*#########################################################################################################################*/
// With fog, vertex alpha is how visible the vertex is through the fog, so it mustn't be used
//  as the alpha of the pixel (otherwise e.g. fogged blocks would be discarded by alpha testing)
#define COMBINER_TEX_SHADE_FOG RDPQ_COMBINER1((TEX0, 0, SHADE, 0), (0, 0, 0, TEX0))
#define COMBINER_SHADE_FOG     RDPQ_COMBINER1((0, 0, 0, SHADE),    (0, 0, 0, 1))

static void UpdateCombiner(void) {
	if (gfx_format == VERTEX_FORMAT_TEXTURED) {
		rdpq_mode_combiner(gfx_fogEnabled ? COMBINER_TEX_SHADE_FOG : RDPQ_COMBINER_TEX_SHADE);
	} else {
		rdpq_mode_combiner(gfx_fogEnabled ? COMBINER_SHADE_FOG     : RDPQ_COMBINER_SHADE);
	}
}

void Gfx_SetVertexFormat(VertexFormat fmt) {
	if (fmt == gfx_format) return;
	gfx_format = fmt;
	gfx_stride = strideSizes[fmt];
	gpu_stride = gfx_stride;
	UpdateCombiner();

	gpu_texturing = fmt == VERTEX_FORMAT_TEXTURED;
	gpu_attr_tex = gpu_texturing;
	gpuUpdateFormat();
}

void Gfx_DrawVb_Lines(int verticesCount) {
}


/*########################################################################################################################*
*-----------------------------------------------------Large textures------------------------------------------------------*
*#########################################################################################################################*/
// Textures that don't fit in TMEM are drawn by splitting each quad into pieces whose part of the
//  texture does fit in TMEM, and then uploading that part of the texture before drawing each piece
// NOTE: Only works for quads whose texture coordinates are axis aligned (e.g. GUI elements)

static int LerpI(int a, int b, float t) { return a + (int)((b - a) * t); }

// Bilinearly interpolates position between the four corners of a quad
static void LerpCorners(struct rsp_vertex* dst, struct rsp_vertex* c[2][2], float fu, float fv) {
	#define LERP_POS(comp) \
		LerpI(LerpI((int16_t)c[0][0]->comp, (int16_t)c[1][0]->comp, fu), \
			  LerpI((int16_t)c[0][1]->comp, (int16_t)c[1][1]->comp, fu), fv)

	dst->x = LERP_POS(x);
	dst->y = LERP_POS(y);
	dst->z = LERP_POS(z);
	dst->w = 0;
}

// Calculates the region of texels that a quad's texture coordinates cover
static void CalcQuadTexels(struct rsp_vertex* v, int* s0, int* t0, int* s1, int* t1) {
	int texW = gfx_largeTex->surface.width, texH = gfx_largeTex->surface.height;
	int umin = (int16_t)v[0].u, umax = umin, vmin = (int16_t)v[0].v, vmax = vmin;

	for (int i = 1; i < 4; i++)
	{
		umin = min(umin, (int16_t)v[i].u); umax = max(umax, (int16_t)v[i].u);
		vmin = min(vmin, (int16_t)v[i].v); vmax = max(vmax, (int16_t)v[i].v);
	}

	// Texture coordinates are 1 << TEX_SHIFT for the whole texture
	*s0 = max(0,    (umin * texW) >> TEX_SHIFT);
	*t0 = max(0,    (vmin * texH) >> TEX_SHIFT);
	*s1 = min(texW, (umax * texW + (1 << TEX_SHIFT) - 1) >> TEX_SHIFT);
	*t1 = min(texH, (vmax * texH + (1 << TEX_SHIFT) - 1) >> TEX_SHIFT);
}

static void DrawLargeQuad(struct rsp_vertex* v) {
	CCTexture* tex = gfx_largeTex;
	int texW = tex->surface.width, texH = tex->surface.height;
	int bpp  = TEX_FORMAT_BITDEPTH(surface_get_format(&tex->surface)) / 8;
	struct rsp_vertex* c[2][2] = { 0 };
	struct rsp_vertex quad[4] __attribute__((aligned(8)));
	rdpq_texparms_t params = { 0 };
	int umin, umax, vmin, vmax, i;
	int s0, t0, s1, t1;

	umin = umax = (int16_t)v[0].u;
	vmin = vmax = (int16_t)v[0].v;
	for (i = 1; i < 4; i++)
	{
		umin = min(umin, (int16_t)v[i].u); umax = max(umax, (int16_t)v[i].u);
		vmin = min(vmin, (int16_t)v[i].v); vmax = max(vmax, (int16_t)v[i].v);
	}
	if (umin == umax || vmin == vmax) return;

	// Work out which corner of the texture region each vertex is at
	for (i = 0; i < 4; i++)
	{
		int cu = (int16_t)v[i].u == umax, cv = (int16_t)v[i].v == vmax;
		if ((int16_t)v[i].u != umin && !cu) return;
		if ((int16_t)v[i].v != vmin && !cv) return;
		c[cu][cv] = &v[i];
	}
	if (!c[0][0] || !c[0][1] || !c[1][0] || !c[1][1]) return;

	CalcQuadTexels(v, &s0, &t0, &s1, &t1);
	if (s0 >= s1 || t0 >= t1) return;

	// Find a size of piece that fits in TMEM
	int pieceW = s1 - s0, pieceH = t1 - t0;
	while (!FitsInTMEM(pieceW, pieceH, bpp))
	{
		if (pieceW >= pieceH) { pieceW = (pieceW + 1) / 2; } else { pieceH = (pieceH + 1) / 2; }
	}

	for (int t = t0; t < t1; t += pieceH)
	{
		int tEnd = min(t + pieceH, t1);
		int va   = max(vmin, (t    << TEX_SHIFT) / texH);
		int vb   = min(vmax, (tEnd << TEX_SHIFT) / texH);
		float fa = (float)(va - vmin) / (vmax - vmin);
		float fb = (float)(vb - vmin) / (vmax - vmin);

		for (int s = s0; s < s1; s += pieceW)
		{
			int sEnd = min(s + pieceW, s1);
			int ua   = max(umin, (s    << TEX_SHIFT) / texW);
			int ub   = min(umax, (sEnd << TEX_SHIFT) / texW);
			float ea = (float)(ua - umin) / (umax - umin);
			float eb = (float)(ub - umin) / (umax - umin);

			for (i = 0; i < 4; i++)
			{
				int cu = (int16_t)v[i].u == umax, cv = (int16_t)v[i].v == vmax;
				LerpCorners(&quad[i], c, cu ? eb : ea, cv ? fb : fa);
				quad[i].rgba = v[i].rgba;
				quad[i].u    = cu ? ub : ua;
				quad[i].v    = cv ? vb : va;
			}

			rdpq_tex_upload_sub(TILE0, &tex->surface, &params, s, t, sEnd, tEnd);
			gpu_pointer = quad;
			gpuDrawArrays(0, 4);
		}
	}
}

static void DrawLargeQuads(int startVertex, int verticesCount) {
	struct rsp_vertex* v = (struct rsp_vertex*)gfx_vb->vertices + startVertex;
	int bpp = TEX_FORMAT_BITDEPTH(surface_get_format(&gfx_largeTex->surface)) / 8;
	rdpq_texparms_t params = { 0 };
	int s0, t0, s1, t1, i = 0, j;

	while (i < verticesCount)
	{
		CalcQuadTexels(v + i, &s0, &t0, &s1, &t1);
		if (s0 >= s1 || t0 >= t1 || !FitsInTMEM(s1 - s0, t1 - t0, bpp)) {
			DrawLargeQuad(v + i); i += 4; continue;
		}

		// Usually consecutive quads use nearby parts of the texture (e.g. all the faces of a
		//  model's head), so draw as many quads as possible with a single upload to TMEM
		for (j = i + 4; j < verticesCount; j += 4)
		{
			int a0, b0, a1, b1;
			CalcQuadTexels(v + j, &a0, &b0, &a1, &b1);
			a0 = min(a0, s0); b0 = min(b0, t0);
			a1 = max(a1, s1); b1 = max(b1, t1);

			if (a0 >= a1 || b0 >= b1 || !FitsInTMEM(a1 - a0, b1 - b0, bpp)) break;
			s0 = a0; t0 = b0; s1 = a1; t1 = b1;
		}

		rdpq_tex_upload_sub(TILE0, &gfx_largeTex->surface, &params, s0, t0, s1, t1);
		gpu_pointer = v;
		gpuDrawArrays(i, j - i);
		i = j;
	}
}

static void DrawQuads(int startVertex, int verticesCount) {
	if (gfx_largeTex) { DrawLargeQuads(startVertex, verticesCount); return; }

	// Static vertex buffers don't change, so the RSP can just read their vertices directly
	//  (which avoids copying them into the command queue, or keeping copies of them in blocks)
	if (!gfx_vb->cache.dynamic) {
		gfx_vb->cache.drawn = true;
		gpuDrawQuadsFrom(gfx_vb->vertices, startVertex, verticesCount);
		return;
	}
	rspq_block_t* block = VB_GetCached(gfx_vb, startVertex, verticesCount);

	if (block) {
		rspq_block_run(block);
	} else {
		gpu_pointer = gfx_vb->vertices;
		gpuDrawArrays(startVertex, verticesCount);
	}
}

void Gfx_DrawVb_IndexedTris_Range(int verticesCount, int startVertex, DrawHints hints) {
	if (!gfx_vb) return; // e.g. failed to allocate vertex buffer due to running out of memory
	DrawQuads(startVertex, verticesCount);
}

void Gfx_DrawVb_IndexedTris(int verticesCount) {
	if (!gfx_vb) return;
	DrawQuads(0, verticesCount);
}

void Gfx_DrawIndexedTris_T2fC4b(int verticesCount, int startVertex, DrawHints hints) {
	if (depthOnlyRendering || !gfx_vb) return;
	DrawQuads(startVertex, verticesCount);
}

