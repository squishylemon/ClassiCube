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
#include <malloc.h>
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
*-----------------------------------------------------Save filesystem-----------------------------------------------------*
*#########################################################################################################################*/
// Without an SD card, game data (e.g. options and maps) is stored in the cartridge's FlashRAM instead.
//  As FlashRAM is only 128 KB, this is a very simple filesystem: a table of files at the start,
//  followed by the contents of all the files packed together, which is entirely kept in RAM
//  and written back to FlashRAM whenever a file has been written.
// NOTE: Files are only kept in RAM when there's no FlashRAM (e.g. emulator set to another save type)
#define SAVE_PREFIX      "save:/"
#define SAVE_PREFIX_LEN  6
#define SAVE_SIZE        (128 * 1024)
#define SAVE_MAX_FILES   31
#define SAVE_NAME_LEN    56
#define SAVE_MAGIC       0x43435346 // "CCSF"
#define SAVE_FD_BASE     0x4000
#define SAVE_MAX_HANDLES 4

struct SaveEntry  { char name[SAVE_NAME_LEN]; cc_uint32 offset, size; };
struct SaveHeader { cc_uint32 magic, count, _pad[14]; struct SaveEntry files[SAVE_MAX_FILES]; };

struct SaveHandle {
	cc_bool used, writing;
	char name[SAVE_NAME_LEN];
	cc_uint8* data; // Points into save_image when reading, separate buffer when writing
	cc_uint32 size, capacity, pos;
};

static cc_uint8* save_image;
static cc_bool save_hasFlash;
static struct SaveHandle save_handles[SAVE_MAX_HANDLES];
#define SaveFS_Header() ((struct SaveHeader*)save_image)
#define SaveFS_IsPath(path) (!strncmp(path, SAVE_PREFIX, SAVE_PREFIX_LEN))
#define SaveFS_IsFile(file) ((file) >= SAVE_FD_BASE && (file) < SAVE_FD_BASE + SAVE_MAX_HANDLES)
#define SaveFS_Handle(file) (&save_handles[(file) - SAVE_FD_BASE])

static cc_bool SaveFS_IsValid(struct SaveHeader* hdr) {
	cc_uint32 end = sizeof(struct SaveHeader);
	if (hdr->magic != SAVE_MAGIC || hdr->count > SAVE_MAX_FILES) return false;

	for (int i = 0; i < hdr->count; i++)
	{
		struct SaveEntry* e = &hdr->files[i];
		if (e->offset != end || e->size > SAVE_SIZE - end) return false;
		if (e->name[SAVE_NAME_LEN - 1]) return false;
		end += e->size;
	}
	return true;
}

static void SaveFS_Init(void) {
	save_image = memalign(16, SAVE_SIZE);
	if (!save_image) return;
	Mem_Set(save_image, 0, SAVE_SIZE);

	save_hasFlash = flashram_init(NULL, NULL);
	if (save_hasFlash) flashram_read(save_image, 0, SAVE_SIZE);
	if (!save_hasFlash) Platform_LogConst("No FlashRAM, so data won't be saved");
	// Kernel isn't running yet, so this polls until the PI is idle (see SaveFS_WriteFlash)
	dma_wait();

	if (!SaveFS_IsValid(SaveFS_Header())) {
		Mem_Set(save_image, 0, sizeof(struct SaveHeader));
		SaveFS_Header()->magic = SAVE_MAGIC;
	}
}

static int SaveFS_Find(const char* name) {
	struct SaveHeader* hdr = SaveFS_Header();
	for (int i = 0; i < hdr->count; i++)
	{
		if (!strcmp(hdr->files[i].name, name)) return i;
	}
	return -1;
}

static cc_uint32 SaveFS_UsedSize(void) {
	struct SaveHeader* hdr = SaveFS_Header();
	if (!hdr->count) return sizeof(struct SaveHeader);

	struct SaveEntry* last = &hdr->files[hdr->count - 1];
	return last->offset + last->size;
}

static cc_result SaveFS_Open(cc_file* file, const char* path, cc_bool write, cc_bool truncate) {
	const char* name = path + SAVE_PREFIX_LEN;
	struct SaveHandle* h = NULL;
	int idx, i;
	if (!save_image) return ENOMEM;
	if (String_Length(name) >= SAVE_NAME_LEN) return ENAMETOOLONG;

	idx = SaveFS_Find(name);
	if (idx < 0 && !write) return ENOENT;

	for (i = 0; i < SAVE_MAX_HANDLES; i++)
	{
		if (save_handles[i].used) continue;
		h = &save_handles[i]; break;
	}
	if (!h) return EMFILE;
	Mem_Set(h, 0, sizeof(*h));

	h->used    = true;
	h->writing = write;
	strcpy(h->name, name);

	if (!write) {
		h->data = save_image + SaveFS_Header()->files[idx].offset;
		h->size = SaveFS_Header()->files[idx].size;
	} else if (idx >= 0 && !truncate) {
		struct SaveEntry* e = &SaveFS_Header()->files[idx];
		h->data = Mem_TryAlloc(max(e->size, 1), 1);
		if (!h->data) { h->used = false; return ENOMEM; }

		Mem_Copy(h->data, save_image + e->offset, e->size);
		h->size = h->capacity = e->size;
	}

	*file = SAVE_FD_BASE + i;
	return 0;
}

static cc_result SaveFS_Read(struct SaveHandle* h, void* data, cc_uint32 count, cc_uint32* bytesRead) {
	count = min(count, h->size - min(h->pos, h->size));
	Mem_Copy(data, h->data + h->pos, count);

	h->pos    += count;
	*bytesRead = count;
	return 0;
}

static cc_result SaveFS_Write(struct SaveHandle* h, const void* data, cc_uint32 count, cc_uint32* bytesWrote) {
	cc_uint32 end = h->pos + count;
	*bytesWrote   = 0;
	if (!h->writing)     return EBADF;
	if (end > SAVE_SIZE) return ENOSPC;

	if (end > h->capacity) {
		cc_uint32 capacity = max(end, h->capacity * 2);
		cc_uint8* buffer   = Mem_TryRealloc(h->data, capacity, 1);
		if (!buffer) return ENOMEM;

		h->data     = buffer;
		h->capacity = capacity;
	}

	Mem_Copy(h->data + h->pos, data, count);
	h->pos  = end;
	h->size = max(h->size, end);
	*bytesWrote = count;
	return 0;
}

// Workaround for a libdragon bug: When the kernel is running, waiting for the PI to finish is done
//  by waiting for a PI interrupt. However, writing a FlashRAM command (unlike DMA) doesn't raise
//  one when finished, so the next command can end up waiting forever. So pretend the kernel isn't
//  running while writing, which makes libdragon instead poll the PI until it's finished.
// (other threads don't run in the meantime, and are woken up again by later interrupts)
// NOTE: The PI must also be idle afterwards, as otherwise e.g. the next ROM read would hang too
extern bool __kernel;

static int SaveFS_WriteFlash(cc_uint32 len) {
	__kernel = false;
	// NOTE: FlashRAM writes must be an even number of bytes
	int ret  = flashram_write(save_image, 0, CC_ALIGNUP(len, 2));
	dma_wait();
	__kernel = true;
	return ret;
}

// Replaces the file's contents in the save image, and then writes that to FlashRAM
static cc_result SaveFS_Commit(struct SaveHandle* h) {
	struct SaveHeader* hdr = SaveFS_Header();
	cc_uint32 end = SaveFS_UsedSize();
	int idx = SaveFS_Find(h->name);
	struct SaveEntry* e;

	cc_uint32 oldSize = idx >= 0 ? hdr->files[idx].size : 0;
	if (idx < 0 && hdr->count >= SAVE_MAX_FILES)  return ENOSPC;
	if (end - oldSize + h->size > SAVE_SIZE)      return ENOSPC;

	// Remove the old contents, moving all the files after it back
	if (idx >= 0) {
		e = &hdr->files[idx];
		Mem_Move(save_image + e->offset, save_image + e->offset + e->size, end - (e->offset + e->size));
		end -= e->size;

		for (int i = idx + 1; i < hdr->count; i++)
		{
			hdr->files[i].offset -= e->size;
			hdr->files[i - 1]     = hdr->files[i];
		}
		hdr->count--;
	}

	e = &hdr->files[hdr->count++];
	Mem_Set(e->name, 0, SAVE_NAME_LEN);
	strcpy(e->name, h->name);
	e->offset = end;
	e->size   = h->size;
	Mem_Copy(save_image + end, h->data, h->size);
	end += h->size;

	if (!save_hasFlash) return 0;
	return SaveFS_WriteFlash(end) < 0 ? EIO : 0;
}

static cc_result SaveFS_Close(struct SaveHandle* h) {
	cc_result res = 0;
	if (h->writing) {
		res = SaveFS_Commit(h);
		Mem_Free(h->data);
	}

	h->used = false;
	return res;
}

static cc_result SaveFS_Enum(const cc_string* dirPath, void* obj, Directory_EnumCallback callback) {
	cc_string path; char pathBuffer[FILENAME_SIZE];
	struct SaveHeader* hdr = SaveFS_Header();
	if (!save_image) return ENOMEM;

	for (int i = 0; i < hdr->count; i++)
	{
		cc_string name = String_FromReadonly(hdr->files[i].name);
		// Only files directly in the directory (e.g. "maps/abc.cw" for "maps")
		if (name.length <= dirPath->length || name.buffer[dirPath->length] != '/') continue;
		if (!String_CaselessStarts(&name, dirPath)) continue;
		if (String_IndexOfAt(&name, dirPath->length + 1, '/') >= 0) continue;

		String_InitArray(path, pathBuffer);
		String_AppendString(&path, &name);
		callback(&path, obj, false);
	}
	return 0;
}


/*########################################################################################################################*
*-----------------------------------------------------Directory/File------------------------------------------------------*
*#########################################################################################################################*/
static cc_string root_path = String_FromConst("sd:/ClassiCube/");
static cc_string root_path_save = String_FromConst(SAVE_PREFIX);

void Platform_EncodePath(cc_filepath* dst, const cc_string* path) {
	char* str = dst->buffer;

	// Paths that already start with a device (e.g. "rom:/") aren't relative to the root directory
	if (String_IndexOf(path, ':') == -1) {
		Mem_Copy(str, root_path.buffer, root_path.length);
		str += root_path.length;
	}
	String_EncodeUtf8(str, path);
}

const char* Platform_BundledMusicDir(void) { return "rom:/music/"; }

void Platform_DecodePath(cc_string* dst, const cc_filepath* path) {
	const char* str = path->buffer;
	String_AppendUtf8(dst, str, String_Length(str));
}

void Directory_GetCachePath(cc_string* path) { }

cc_result Directory_Create2(const cc_filepath* path) {
	// Save filesystem has no directories, only files with '/' in their names
	if (SaveFS_IsPath(path->buffer)) return 0;
	return mkdir(path->buffer, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) == -1 ? errno : 0;
}

int File_Exists(const cc_filepath* path) {
	if (SaveFS_IsPath(path->buffer)) {
		return save_image && SaveFS_Find(path->buffer + SAVE_PREFIX_LEN) >= 0;
	}

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
	if (SaveFS_IsPath(str.buffer)) return SaveFS_Enum(dirPath, obj, callback);

	int res = dir_findfirst(str.buffer, &entry);
	if (res != 0) return errno;

	errno = 0;
	String_InitArray(path, pathBuffer);

	for (; res == 0; res = dir_findnext(str.buffer, &entry)) {
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
	}

	return errno; // no clue if libdragon actually sets errno...
}

static cc_result File_Do(cc_file* file, const char* path, int mode) {
	*file = open(path, mode, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	return *file == -1 ? errno : 0;
}

cc_result File_Open(cc_file* file, const cc_filepath* path) {
	if (SaveFS_IsPath(path->buffer)) return SaveFS_Open(file, path->buffer, false, false);
	return File_Do(file, path->buffer, O_RDONLY);
}
cc_result File_Create(cc_file* file, const cc_filepath* path) {
	if (SaveFS_IsPath(path->buffer)) return SaveFS_Open(file, path->buffer, true,  true);
	return File_Do(file, path->buffer, O_RDWR | O_CREAT | O_TRUNC);
}
cc_result File_OpenOrCreate(cc_file* file, const cc_filepath* path) {
	if (SaveFS_IsPath(path->buffer)) return SaveFS_Open(file, path->buffer, true,  false);
	return File_Do(file, path->buffer, O_RDWR | O_CREAT);
}

cc_result File_Read(cc_file file, void* data, cc_uint32 count, cc_uint32* bytesRead) {
	cc_uint8 tmp[512] __attribute__((aligned(16)));
	cc_uint8* dst = (cc_uint8*)data;
	int read_ = 0, n;
	if (SaveFS_IsFile(file)) return SaveFS_Read(SaveFS_Handle(file), data, count, bytesRead);

	// ROM files are read using DMA, which needs the destination to be whole cache lines.
	//  Otherwise if another thread accesses memory sharing a cache line with the start or
	//  end of the destination during the DMA, the data read can end up partly stale
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
	if (SaveFS_IsFile(file)) return SaveFS_Write(SaveFS_Handle(file), data, count, bytesWrote);

	*bytesWrote = write(file, data, count);
	return *bytesWrote == -1 ? errno : 0;
}

cc_result File_Close(cc_file file) {
	if (SaveFS_IsFile(file)) return SaveFS_Close(SaveFS_Handle(file));
	return close(file) == -1 ? errno : 0;
}

cc_result File_Seek(cc_file file, int offset, int seekType) {
	static cc_uint8 modes[3] = { SEEK_SET, SEEK_CUR, SEEK_END };

	if (SaveFS_IsFile(file)) {
		struct SaveHandle* h = SaveFS_Handle(file);
		int base = seekType == FILE_SEEKFROM_BEGIN ? 0 : (seekType == FILE_SEEKFROM_CURRENT ? h->pos : h->size);
		if (base + offset < 0) return EINVAL;

		h->pos = base + offset;
		return 0;
	}
	return lseek(file, offset, modes[seekType]) == -1 ? errno : 0;
}

cc_result File_Position(cc_file file, cc_uint32* pos) {
	if (SaveFS_IsFile(file)) { *pos = SaveFS_Handle(file)->pos; return 0; }

	*pos = lseek(file, 0, SEEK_CUR);
	return *pos == -1 ? errno : 0;
}

cc_result File_Length(cc_file file, cc_uint32* len) {
	struct stat st;
	if (SaveFS_IsFile(file)) { *len = SaveFS_Handle(file)->size; return 0; }

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

struct ThreadData {
	kthread_t* thread;
	Thread_StartFunc func;
	volatile cc_bool finished;
};

static int ExecThread(void* param) {
	struct ThreadData* data = (struct ThreadData*)param;
	// Each thread has its own FPU control register, which starts out as 0 for new threads
	//  (i.e. denormals aren't flushed to zero, and cause an exception instead)
	DisableFpuExceptions();
	data->func();
	data->finished = true;
	return 0;
}

// Worker threads (e.g. music) run at a higher priority than the main thread, so that
//  they still get to run (whenever they wake up) while the main thread is busy rendering
#define WORKER_THREAD_PRIORITY 1

void Thread_Run(void** handle, Thread_StartFunc func, int stackSize, const char* name) {
	struct ThreadData* data = (struct ThreadData*)Mem_Alloc(1, sizeof(struct ThreadData), "thread");
	data->func     = func;
	data->finished = false;

	// Requested stack sizes are meant for desktop systems, and would waste a lot of RAM
	stackSize    = min(stackSize, 32 * 1024);
	data->thread = kthread_new(name, stackSize, WORKER_THREAD_PRIORITY, ExecThread, data);
	if (!data->thread) Process_Abort("Creating thread");
	*handle = data;
}

void Thread_Detach(void* handle) {
	struct ThreadData* data = (struct ThreadData*)handle;
	// NOTE: data isn't freed, as the thread might still be using it (e.g. it detached itself)
	if (data) kthread_detach(data->thread);
}

// Workaround for a libdragon bug: Joining a thread that hasn't finished yet fails an assertion
//  (the joining thread isn't marked as waiting), but joining a finished thread works fine
void Thread_Join(void* handle) {
	struct ThreadData* data = (struct ThreadData*)handle;
	if (!data) return;

	// Thread has a higher priority, so has finished exiting by the time this thread runs again
	while (!data->finished) WaitVBlanks(16);
	kthread_join(data->thread);
	Mem_Free(data);
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
		Platform_LogConst("Couldn't access SD card, saving to FlashRAM instead");
		String_Copy(&root_path, &root_path_save);
		SaveFS_Init();
		// Save filesystem is far too small for e.g. chat logs or a texture pack cache
		Platform_ReadonlyFilesystem = true;
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

