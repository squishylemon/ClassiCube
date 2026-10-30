#include "../Window.h"
#include "../Platform.h"
#include "../Input.h"
#include "../InputHandler.h"
#include "../Event.h"
#include "../Graphics.h"
#include "../String_.h"
#include "../Funcs.h"
#include "../Bitmap.h"
#include "../Errors.h"
#include "../ExtMath.h"
// Default on-screen keyboard is mostly transparent, which makes it hard to read
//  on a TV (especially with chat behind it), so use solid keys and an obvious selection
// (key labels are drawn in black)
#define KB_NORMAL_COLOR     BitmapCol_Make(0xB8, 0xB8, 0xB8, 0xF0)
#define KB_SELECTED_COLOR   BitmapCol_Make(0xF0, 0xC0, 0x30, 0xFF)
#define KB_BACKGROUND_COLOR BitmapCol_Make(0x30, 0x30, 0x30, 0xFF)
#include "../VirtualKeyboard.h"
#include "../VirtualDialog.h"
#include "../Options.h"
#include "../VirtualCursor.h"

#include <libdragon.h>

struct _DisplayData DisplayInfo;
struct cc_window WindowInfo;

// Workaround for a libdragon bug: When an interrupt handler uses the FPU, the FPU registers of
//  the interrupted thread are first saved into its interrupt frame. However, if the interrupt
//  also wakes up another thread, the FPU registers are saved into the frame *again* before
//  switching threads, overwriting the correct values with what the handler left in them.
// The display's VI handler uses the FPU, so this restores the interrupted thread's FPU registers
//  after it has run. (VI handlers are called in the reverse order they were registered in,
//  so this must be registered before display_init registers the display's VI handler)
extern void* interrupt_exception_frame; // libdragon inthandler.S
#define FRAME_FC31 "316"
#define FRAME_FPR(i) #i "*8+320"

static void RestoreFpuRegisters(void) {
	void* frame = interrupt_exception_frame;
	// FPU is only enabled in the interrupt handler after the FPU registers were saved
	if (!frame || !(C0_STATUS() & C0_STATUS_CU1)) return;

	__asm__ volatile(
		"lw   $t0, " FRAME_FC31 "(%0)\n"
		"ctc1 $t0, $31\n"
		"ldc1 $f0,  " FRAME_FPR(0)  "(%0)\n" "ldc1 $f1,  " FRAME_FPR(1)  "(%0)\n"
		"ldc1 $f2,  " FRAME_FPR(2)  "(%0)\n" "ldc1 $f3,  " FRAME_FPR(3)  "(%0)\n"
		"ldc1 $f4,  " FRAME_FPR(4)  "(%0)\n" "ldc1 $f5,  " FRAME_FPR(5)  "(%0)\n"
		"ldc1 $f6,  " FRAME_FPR(6)  "(%0)\n" "ldc1 $f7,  " FRAME_FPR(7)  "(%0)\n"
		"ldc1 $f8,  " FRAME_FPR(8)  "(%0)\n" "ldc1 $f9,  " FRAME_FPR(9)  "(%0)\n"
		"ldc1 $f10, " FRAME_FPR(10) "(%0)\n" "ldc1 $f11, " FRAME_FPR(11) "(%0)\n"
		"ldc1 $f12, " FRAME_FPR(12) "(%0)\n" "ldc1 $f13, " FRAME_FPR(13) "(%0)\n"
		"ldc1 $f14, " FRAME_FPR(14) "(%0)\n" "ldc1 $f15, " FRAME_FPR(15) "(%0)\n"
		"ldc1 $f16, " FRAME_FPR(16) "(%0)\n" "ldc1 $f17, " FRAME_FPR(17) "(%0)\n"
		"ldc1 $f18, " FRAME_FPR(18) "(%0)\n" "ldc1 $f19, " FRAME_FPR(19) "(%0)\n"
		: : "r"(frame) : "t0", "memory",
		"$f0", "$f1", "$f2",  "$f3",  "$f4",  "$f5",  "$f6",  "$f7",  "$f8",  "$f9",
		"$f10", "$f11", "$f12", "$f13", "$f14", "$f15", "$f16", "$f17", "$f18", "$f19");
}

void Window_PreInit(void) {
	// Without the Expansion Pak there's only 4 MB of RAM, so use 16 bit framebuffers to save memory
	cc_bool expanded = is_memory_expanded();
	register_VI_handler(RestoreFpuRegisters);
	display_init(RESOLUTION_320x240, expanded ? DEPTH_32_BPP : DEPTH_16_BPP, expanded ? 3 : 2, GAMMA_NONE, FILTERS_DISABLED);
	//display_init(RESOLUTION_320x240, DEPTH_32_BPP, 3, GAMMA_NONE, ANTIALIAS_RESAMPLE_FETCH_ALWAYS);    
}

void Window_Init(void) {
	DisplayInfo.Width  = display_get_width();
	DisplayInfo.Height = display_get_height();
	DisplayInfo.ScaleX = 0.5f;
	DisplayInfo.ScaleY = 0.5f;
	
	Window_Main.Width    = DisplayInfo.Width;
	Window_Main.Height   = DisplayInfo.Height;
	Window_Main.Focused  = true;
	
	Window_Main.Exists   = true;
	Window_Main.UIScaleX = DEFAULT_UI_SCALE_X;
	Window_Main.UIScaleY = DEFAULT_UI_SCALE_Y;

	DisplayInfo.ContentOffsetX = Option_GetOffsetX(10);
	DisplayInfo.ContentOffsetY = Option_GetOffsetY(10);
	Window_Main.SoftKeyboard   = SOFT_KEYBOARD_VIRTUAL;
}

void Window_Free(void) { }

void Window_Create2D(int width, int height) { Window_Main.Is3D = false; }
void Window_Create3D(int width, int height) { Window_Main.Is3D = true;  }

void Window_Destroy(void) { }

void Window_SetTitle(const cc_string* title) { }
void Clipboard_GetText(cc_string* value) { }
void Clipboard_SetText(const cc_string* value) { }

int Window_GetWindowState(void) { return WINDOW_STATE_FULLSCREEN; }
cc_result Window_EnterFullscreen(void) { return 0; }
cc_result Window_ExitFullscreen(void)  { return 0; }
int Window_IsObscured(void)            { return 0; }

void Window_Show(void) { }
void Window_SetSize(int width, int height) { }

void Window_RequestClose(void) {
	Event_RaiseVoid(&WindowEvents.Closing);
}


/*########################################################################################################################*
*----------------------------------------------------Input processing-----------------------------------------------------*
*#########################################################################################################################*/
void Window_ProcessEvents(float delta) {
}

void Window_EnableRawMouse(void)  { Input.RawMode = true;  }
void Window_DisableRawMouse(void) { Input.RawMode = false; }
void Window_UpdateRawMouse(void)  { }

static void ProcessMouse(joypad_inputs_t* inputs, float delta) {
	Input_SetNonRepeatable(CCMOUSE_L, inputs->btn.a);
	Input_SetNonRepeatable(CCMOUSE_R, inputs->btn.b);

	// TODO check stick_x/y is right
	VirtualCursor_Update(inputs->stick_x, inputs->stick_y, delta);
}


/*########################################################################################################################*
*-------------------------------------------------------Gamepads----------------------------------------------------------*
*#########################################################################################################################*/
static const BindMapping defaults_n64[BIND_COUNT] = {
	[BIND_FORWARD] = { CCPAD_CUP    },
	[BIND_BACK]    = { CCPAD_CDOWN  },
	[BIND_LEFT]    = { CCPAD_CLEFT  },
	[BIND_RIGHT]   = { CCPAD_CRIGHT },
	
	[BIND_FLY_UP]  = { CCPAD_UP    },
	[BIND_FLY_DOWN]= { CCPAD_DOWN  },
	[BIND_SPEED]   = { CCPAD_LEFT  },
	[BIND_FLY]     = { CCPAD_RIGHT },
	
	[BIND_JUMP]         = { CCPAD_1 },
	[BIND_INVENTORY]    = { CCPAD_2 },
	[BIND_PLACE_BLOCK]  = { CCPAD_5 },
	[BIND_HOTBAR_RIGHT] = { CCPAD_L },
	[BIND_DELETE_BLOCK] = { CCPAD_R },
	
	[BIND_SET_SPAWN]    = { CCPAD_START },
};

void Gamepads_PreInit(void) {
	joypad_init();
}

void Gamepads_Init(void) { }

static void HandleButtons(int port, joypad_buttons_t btns) {
	Gamepad_SetButton(port, CCPAD_L, btns.l);
	Gamepad_SetButton(port, CCPAD_R, btns.r);
	
	Gamepad_SetButton(port, CCPAD_1, btns.a);
	Gamepad_SetButton(port, CCPAD_2, btns.b);
	Gamepad_SetButton(port, CCPAD_5, btns.z);
	
	Gamepad_SetButton(port, CCPAD_START,  btns.start);
	
	Gamepad_SetButton(port, CCPAD_LEFT,   btns.d_left);
	Gamepad_SetButton(port, CCPAD_RIGHT,  btns.d_right);
	Gamepad_SetButton(port, CCPAD_UP,     btns.d_up);
	Gamepad_SetButton(port, CCPAD_DOWN,   btns.d_down);

	Gamepad_SetButton(port, CCPAD_CLEFT,  btns.c_left);
	Gamepad_SetButton(port, CCPAD_CRIGHT, btns.c_right);
	Gamepad_SetButton(port, CCPAD_CUP,    btns.c_up);
	Gamepad_SetButton(port, CCPAD_CDOWN,  btns.c_down);
}

#define AXIS_SCALE 8.0f
static void ProcessAnalogInput(int port, joypad_inputs_t* inputs, float delta) {
	int x = inputs->stick_x;
	int y = inputs->stick_y;

	if (Math_AbsI(x) <= 8) x = 0;
	if (Math_AbsI(y) <= 8) y = 0;
	
	Gamepad_SetAxis(port, PAD_AXIS_RIGHT, x / AXIS_SCALE, -y / AXIS_SCALE, delta);
}

void Gamepads_Process(float delta) {
	joypad_poll();

	for (int i = 0; i < JOYPAD_PORT_COUNT; i++)
	{
		if (!joypad_is_connected(i)) continue;
		joypad_inputs_t inputs = joypad_get_inputs(i);
		
		if (joypad_get_style(i) == JOYPAD_STYLE_MOUSE) {
			ProcessMouse(&inputs, delta); continue;
		}

		int port = Gamepad_Connect(0x64 + i, defaults_n64);
		HandleButtons(port, inputs.btn);
		ProcessAnalogInput(port, &inputs, delta);
	}
}


/*########################################################################################################################*
*------------------------------------------------------Framebuffer--------------------------------------------------------*
*#########################################################################################################################*/
void Window_AllocFramebuffer(struct Bitmap* bmp, int width, int height) {
	// NOTE: Can fail when out of memory (e.g. when showing an out of memory dialog)
	bmp->scan0  = (BitmapCol*)Mem_TryAlloc(width * height, BITMAPCOLOR_SIZE);
	bmp->width  = width;
	bmp->height = height;
}

void Window_DrawFramebuffer(Rect2D r, struct Bitmap* bmp) {
	surface_t* fb  = display_get();
	cc_uint32* src = (cc_uint32*)bmp->scan0;
	cc_uint8*  dst = (cc_uint8*)fb->buffer;

	if (surface_get_format(fb) == FMT_RGBA16) {
		// 16 bpp requires reducing R8G8B8A8 to R5G5B5A1
		for (int y = 0; y < bmp->height; y++)
		{
			cc_uint32* src_row = src + y * bmp->width;
			cc_uint16* dst_row = (cc_uint16*)(dst + y * fb->stride);

			for (int x = 0; x < bmp->width; x++)
			{
				cc_uint32 c = src_row[x];
				dst_row[x]  = ((c >> 16) & 0xF800) | ((c >> 13) & 0x07C0) | ((c >> 10) & 0x003E) | 1;
			}
		}
	} else {
		for (int y = 0; y < bmp->height; y++)
		{
			Mem_Copy(dst + y * fb->stride,
					 src + y * bmp->width,
					 bmp->width * 4);
		}
	}

    display_show(fb);
}

void Window_FreeFramebuffer(struct Bitmap* bmp) {
	Mem_Free(bmp->scan0);
}


/*########################################################################################################################*
*------------------------------------------------------Soft keyboard------------------------------------------------------*
*#########################################################################################################################*/
void OnscreenKeyboard_Open(struct OpenKeyboardArgs* args) {
	kb_tileWidth  = KB_TILE_SIZE / 2;
	kb_tileHeight = KB_TILE_SIZE / 2;
	VirtualKeyboard_Open(args);
	// Start with a key selected, so it's obvious where the D-pad cursor is
	kb_curX = 0;
}

void OnscreenKeyboard_SetText(const cc_string* text) {
	VirtualKeyboard_SetText(text);
}

void OnscreenKeyboard_Close(void) {
	VirtualKeyboard_Close();
}


/*########################################################################################################################*
*-------------------------------------------------------Misc/Other--------------------------------------------------------*
*#########################################################################################################################*/
void Window_ShowDialog(const char* title, const char* msg) {
	// A 3D frame might be in progress (e.g. maps are generated in between Gfx_BeginFrame
	//  and Gfx_EndFrame), which must be finished before drawing directly to the display
	cc_bool inFrame = rdpq_is_attached();
	if (inFrame) Gfx_EndFrame();

	VirtualDialog_Show(title, msg, false);
	if (inFrame) Gfx_BeginFrame();
}

cc_result Window_OpenFileDialog(const struct OpenFileDialogArgs* args) {
	return ERR_NOT_SUPPORTED;
}

cc_result Window_SaveFileDialog(const struct SaveFileDialogArgs* args) {
	return ERR_NOT_SUPPORTED;
}

