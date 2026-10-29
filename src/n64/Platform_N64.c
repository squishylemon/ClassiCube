#define CC_NO_UPDATER
#define CC_NO_DYNLIB
#define CC_NO_SOCKETS
#define CC_NO_OPEN
#define CC_NO_CRASHHANDLER

#define CC_XTEA_ENCRYPTION
#include "../Stream.h"
#include "../ExtMath.h"
#include "../Funcs.h"
#include "../Window.h"
#include "../Utils.h"
#include "../Errors.h"
#include "../Options.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <libdragon.h>
#include <cop1.h>
//#include <dragonfs.h>
//#include <rtc.h>
//#include <timer.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/fcntl.h>

const cc_result ReturnCode_FileShareViolation = 1000000000; // not used
const cc_result ReturnCode_FileNotFound     = ENOENT;
const cc_result ReturnCode_PathNotFound     = 99999;
const cc_result ReturnCode_DirectoryExists  = EEXIST;

const char* Platform_AppNameSuffix  = " N64";
cc_bool Platform_ReadonlyFilesystem = false;
cc_uint8 Platform_Flags = PLAT_FLAG_SINGLE_PROCESS | PLAT_FLAG_APP_EXIT;
#include "../_PlatformBase.h"


/*########################################################################################################################*
*-----------------------------------------------------Main entrypoint-----------------------------------------------------*
*#########################################################################################################################*/
#include "../main_impl.h"

int main(int argc, char** argv) {
	SetupProgram(argc, argv);
	while (Window_Main.Exists) { 
		RunProgram(argc, argv);
	}
	
	Window_Free();
	return 0;
}


/*########################################################################################################################*
*------------------------------------------------------Logging/Time-------------------------------------------------------*
*#########################################################################################################################*/
cc_uint64 Stopwatch_ElapsedMicroseconds(cc_uint64 beg, cc_uint64 end) {
	if (end < beg) return 0;
	
	cc_uint64 delta = end - beg;
	return TIMER_MICROS_LL(delta);
}

cc_uint64 Stopwatch_Measure(void) {
	return timer_ticks();
}

void Platform_Log(const char* msg, int len) {
	write(STDERR_FILENO, msg,  len);
	write(STDERR_FILENO, "\n",   1);
}

TimeMS DateTime_CurrentUTC(void) {
	return 0;
}

void DateTime_CurrentLocal(struct cc_datetime* t) {
	rtc_time_t curTime = { 0 };
	rtc_get(&curTime);

	t->year   = curTime.year;
	t->month  = curTime.month;
	t->day    = curTime.day;
	t->hour   = curTime.hour;
	t->minute = curTime.min;
	t->second = curTime.sec;
}


/*########################################################################################################################*
*-----------------------------------------------------Directory/File------------------------------------------------------*
*#########################################################################################################################*/
static cc_string root_path = String_FromConst("sd:/ClassiCube/");
static cc_string root_path_rom = String_FromConst("rom:/");

void Platform_EncodePath(cc_filepath* dst, const cc_string* path) {
	char* str = dst->buffer;
	cc_string path_ = *path;

	// Paths that already start with a device (e.g. "rom:/") aren't relative to the root directory
	if (String_IndexOf(path, ':') >= 0) {
		String_EncodeUtf8(str, &path_); return;
	}

	if (Platform_ReadonlyFilesystem) { // read from rom root
		int idx = String_IndexOf(path, '/');
		if (idx >= 0) path_ = String_UNSAFE_SubstringAt(&path_, idx + 1);
	}
	
	Mem_Copy(str, root_path.buffer, root_path.length);
	str += root_path.length;
	String_EncodeUtf8(str, &path_);
}

const char* Platform_BundledMusicDir(void) { return "rom:/music/"; }

void Platform_DecodePath(cc_string* dst, const cc_filepath* path) {
	const char* str = path->buffer;
	String_AppendUtf8(dst, str, String_Length(str));
}

void Directory_GetCachePath(cc_string* path) { }

cc_result Directory_Create2(const cc_filepath* path) {
	return mkdir(path->buffer, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) == -1 ? errno : 0;
}

int File_Exists(const cc_filepath* path) {
	// NOTE: Not all of libdragon's filesystems support stat (e.g. DragonFS doesn't)
	int fd = open(path->buffer, O_RDONLY);
	if (fd < 0) return false;

	close(fd);
	return true;
}

cc_result Directory_Enum(const cc_string* dirPath, void* obj, Directory_EnumCallback callback) {
	cc_string path; char pathBuffer[FILENAME_SIZE];
	cc_filepath str;
	dir_t entry;

	Platform_EncodePath(&str, dirPath);
	int res = dir_findfirst(str.buffer, &entry);
	if (res != 0) return errno;

	errno = 0;
	String_InitArray(path, pathBuffer);

	while (res == 0) {
		path.length = 0;
		String_Format1(&path, "%s/", dirPath);

		/* ignore . and .. entry */
		char* src = entry.d_name;
		if (src[0] == '.' && src[1] == '\0') continue;
		if (src[0] == '.' && src[1] == '.' && src[2] == '\0') continue;

		int len = String_Length(src);
		String_AppendUtf8(&path, src, len);

		int is_dir = entry.d_type == DT_DIR;
		callback(&path, obj, is_dir);
		errno = 0;
		res = dir_findnext(str.buffer, &entry);
	}

	return errno; // no clue if libdragon actually sets errno...
}

static cc_result File_Do(cc_file* file, const char* path, int mode) {
	*file = open(path, mode, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	return *file == -1 ? errno : 0;
}

cc_result File_Open(cc_file* file, const cc_filepath* path) {
	return File_Do(file, path->buffer, O_RDONLY);
}
cc_result File_Create(cc_file* file, const cc_filepath* path) {
	return File_Do(file, path->buffer, O_RDWR | O_CREAT | O_TRUNC);
}
cc_result File_OpenOrCreate(cc_file* file, const cc_filepath* path) {
	return File_Do(file, path->buffer, O_RDWR | O_CREAT);
}

cc_result File_Read(cc_file file, void* data, cc_uint32 count, cc_uint32* bytesRead) {
	cc_uint8 tmp[512] __attribute__((aligned(16)));
	cc_uint8* dst = (cc_uint8*)data;
	int read_ = 0, n;

	// ROM files are read using DMA, which needs the destination to be whole cache lines.
	//  Otherwise memory around the destination can get corrupted when another thread
	//  writes to it while the DMA is in progress, so read those through a temp buffer
	if (((cc_uintptr)data | count) & 15) {
		while (count) {
			n = read(file, tmp, min(count, sizeof(tmp)));
			if (n == -1) return errno;
			if (n == 0)  break;

			Mem_Copy(dst, tmp, n);
			dst += n; read_ += n; count -= n;
		}
		*bytesRead = read_;
		return 0;
	}

	*bytesRead = read(file, data, count);
	return *bytesRead == -1 ? errno : 0;
}

cc_result File_Write(cc_file file, const void* data, cc_uint32 count, cc_uint32* bytesWrote) {
	*bytesWrote = write(file, data, count);
	return *bytesWrote == -1 ? errno : 0;
}

cc_result File_Close(cc_file file) {
	return close(file) == -1 ? errno : 0;
}

cc_result File_Seek(cc_file file, int offset, int seekType) {
	static cc_uint8 modes[3] = { SEEK_SET, SEEK_CUR, SEEK_END };
	return lseek(file, offset, modes[seekType]) == -1 ? errno : 0;
}

cc_result File_Position(cc_file file, cc_uint32* pos) {
	*pos = lseek(file, 0, SEEK_CUR);
	return *pos == -1 ? errno : 0;
}

cc_result File_Length(cc_file file, cc_uint32* len) {
	struct stat st;
	if (fstat(file, &st) == -1) { *len = -1; return errno; }
	*len = st.st_size; return 0;
}


/*########################################################################################################################*
*--------------------------------------------------------Threading--------------------------------------------------------*
*#########################################################################################################################*/
// NOTE: libdragon's kernel only switches threads when the running thread waits/sleeps,
//  or when an interrupt wakes up a higher priority thread (e.g. a sleep finishing)

// Sleeping is done by waiting for vertical blanks rather than with kthread_sleep, as a thread
//  repeatedly sleeping for short periods with kthread_sleep was found to eventually never wake up
static void WaitVBlanks(cc_uint32 milliseconds) {
	int frames = (milliseconds * 60 + 999) / 1000;
	kirq_wait_t wait = kirq_begin_wait_vi();

	for (; frames > 0; frames--) kirq_wait(&wait);
}

void Thread_Sleep(cc_uint32 milliseconds) {
	// VI interrupts only start once the display has been initialised
	if (!display_get_width()) { wait_ms(milliseconds); return; }
	WaitVBlanks(milliseconds);
}

// See src/n64sys.c
static void DisableFpuExceptions(void) {
    uint32_t fcr31 = C1_FCR31();
    
    fcr31 &= ~(C1_ENABLE_OVERFLOW | C1_ENABLE_UNDERFLOW | C1_ENABLE_INEXACT_OP);
    fcr31 |= C1_ENABLE_DIV_BY_0 | C1_ENABLE_INVALID_OP;
    fcr31 |= C1_FCR31_FS; // flush denormals to zero

    C1_WRITE_FCR31(fcr31);	
}

static int ExecThread(void* param) {
	// Each thread has its own FPU control register, which starts out as 0 for new threads
	//  (i.e. denormals aren't flushed to zero, and cause an exception instead)
	DisableFpuExceptions();
	((Thread_StartFunc)param)();
	return 0;
}

// Worker threads (e.g. music) run at a higher priority than the main thread, so that
//  they still get to run (whenever they wake up) while the main thread is busy rendering
#define WORKER_THREAD_PRIORITY 1

void Thread_Run(void** handle, Thread_StartFunc func, int stackSize, const char* name) {
	// Requested stack sizes are meant for desktop systems, and would waste a lot of RAM
	stackSize = min(stackSize, 32 * 1024);
	*handle   = kthread_new(name, stackSize, WORKER_THREAD_PRIORITY, ExecThread, (void*)func);
	if (!*handle) Process_Abort("Creating thread");
}

void Thread_Detach(void* handle) {
	if (handle) kthread_detach((kthread_t*)handle);
}

void Thread_Join(void* handle) {
	if (handle) kthread_join((kthread_t*)handle);
}

void* Mutex_Create(const char* name) {
	kmutex_t* mutex = (kmutex_t*)Mem_Alloc(1, sizeof(kmutex_t), "mutex");
	kmutex_init(mutex, KMUTEX_STANDARD);
	return mutex;
}

void Mutex_Free(void* handle) {
	kmutex_destroy((kmutex_t*)handle);
	Mem_Free(handle);
}

void Mutex_Lock(void* handle)   { kmutex_lock((kmutex_t*)handle); }
void Mutex_Unlock(void* handle) { kmutex_unlock((kmutex_t*)handle); }

struct WaitData {
	kmutex_t mutex;
	kcond_t  cond;
	cc_bool  signalled;
};

void* Waitable_Create(const char* name) {
	struct WaitData* ptr = (struct WaitData*)Mem_Alloc(1, sizeof(struct WaitData), "waitable");
	kmutex_init(&ptr->mutex, KMUTEX_STANDARD);
	kcond_init(&ptr->cond);
	ptr->signalled = false;
	return ptr;
}

void Waitable_Free(void* handle) {
	struct WaitData* ptr = (struct WaitData*)handle;
	kcond_destroy(&ptr->cond);
	kmutex_destroy(&ptr->mutex);
	Mem_Free(ptr);
}

void Waitable_Signal(void* handle) {
	struct WaitData* ptr = (struct WaitData*)handle;
	kmutex_lock(&ptr->mutex);
	ptr->signalled = true;
	kcond_signal(&ptr->cond);
	kmutex_unlock(&ptr->mutex);
}

void Waitable_Wait(void* handle) {
	struct WaitData* ptr = (struct WaitData*)handle;
	kmutex_lock(&ptr->mutex);
	while (!ptr->signalled) kcond_wait(&ptr->cond, &ptr->mutex);
	ptr->signalled = false;
	kmutex_unlock(&ptr->mutex);
}

void Waitable_WaitFor(void* handle, cc_uint32 milliseconds) {
	struct WaitData* ptr = (struct WaitData*)handle;
	// Checks every vertical blank, rather than using kcond_wait_timeout (see Thread_Sleep)
	kirq_wait_t wait = kirq_begin_wait_vi();
	cc_uint64 beg    = Stopwatch_Measure();

	while (!ptr->signalled && Stopwatch_ElapsedMS(beg, Stopwatch_Measure()) < milliseconds)
	{
		kirq_wait(&wait);
	}

	kmutex_lock(&ptr->mutex);
	ptr->signalled = false;
	kmutex_unlock(&ptr->mutex);
}


/*########################################################################################################################*
*--------------------------------------------------------Platform---------------------------------------------------------*
*#########################################################################################################################*/
void Platform_Init(void) {
	debug_init_isviewer();
	debug_init_usblog();
	DisableFpuExceptions();
	
	dfs_init(DFS_DEFAULT_LOCATION);
	timer_init();
	rtc_init();

	if (!debug_init_sdfs("sd:/", -1)) {
		String_Copy(&root_path, &root_path_rom);
		Platform_ReadonlyFilesystem = true;
		Platform_LogConst("Couldn't access SD card.");
	}

	// NOTE: rtc_init and debug_init_sdfs hang when called after the kernel has started
	kernel_init();
}
void Platform_Free(void) { }

cc_bool Platform_DescribeError(cc_result res, cc_string* dst) {
	char chars[NATIVE_STR_LEN];
	int len;

	/* For unrecognised error codes, strerror_r might return messages */
	/*  such as 'No error information', which is not very useful */
	/* (could check errno here but quicker just to skip entirely) */
	if (res >= 1000) return false;

	len = strerror_r(res, chars, NATIVE_STR_LEN);
	if (len == -1) return false;

	len = String_CalcLen(chars, NATIVE_STR_LEN);
	String_AppendUtf8(dst, chars, len);
	return true;
}

void Process_Exit(cc_result code) { exit(code); }

cc_result Process_StartGame2(const cc_string* args, int numArgs) {
	Platform_LogConst("START CLASSICUBE");
	return SetGameArgs(args, numArgs);
}

int Platform_GetCommandLineArgs(int argc, STRING_REF char** argv, cc_string* args) {
	return GetGameArgs(args);
}

cc_result Platform_SetDefaultCurrentDirectory(void) { return 0; }

void CPU_FlushDataCache(void* start, int length) {
	data_cache_hit_writeback(start, length);
}


/*########################################################################################################################*
*-------------------------------------------------------Encryption--------------------------------------------------------*
*#########################################################################################################################*/
#define MACHINE_KEY "N64_N64_N64_N64_"

static cc_result GetMachineID(cc_uint32* key) {
	Mem_Copy(key, MACHINE_KEY, sizeof(MACHINE_KEY) - 1);
	return 0;
}

cc_result Platform_GetEntropy(void* data, int len) {
	return ERR_NOT_SUPPORTED;
}

