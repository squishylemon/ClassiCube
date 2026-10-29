#define CC_XTEA_ENCRYPTION
#define CC_NO_UPDATER
#define CC_NO_DYNLIB
#define CC_NO_OPEN
#define CC_NO_CRASHHANDLER
#define DEFAULT_COMMANDLINE_FUNC

#include "../Stream.h"
#include "../ExtMath.h"
#include "../Funcs.h"
#include "../Window.h"
#include "../Utils.h"
#include "../Errors.h"
#include "../PackedCol.h"

#define LIBCGLUE_SYS_SOCKET_ALIASES 0
#define LIBCGLUE_SYS_SOCKET_NO_ALIASES
#define LIBCGLUE_ARPA_INET_NO_ALIASES
#include <errno.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/time.h>
#include <kernel.h>
#include <delaythread.h>
#include <debug.h>
#include <ps2ip.h>
#include <sifrpc.h>
#include <iopheap.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <sbv_patches.h>
#include <netman.h>
#include <ps2ip.h>
#include <dma.h>
#define NEWLIB_PORT_AWARE
#include <fileio.h>
#include <io_common.h>
#include <iox_stat.h>
#include <libcdvd.h>
#include <libmc.h>

const cc_result ReturnCode_FileShareViolation = 1000000000; // not used
const cc_result ReturnCode_FileNotFound       = -ENOENT;
const cc_result ReturnCode_PathNotFound       = 99999;
const cc_result ReturnCode_DirectoryExists    = -EEXIST;

const cc_result ReturnCode_SocketInProgess  = EINPROGRESS;
const cc_result ReturnCode_SocketWouldBlock = EWOULDBLOCK;
const cc_result ReturnCode_SocketDropped    = EPIPE;

const char* Platform_AppNameSuffix = " PS2";
cc_uint8 Platform_Flags = PLAT_FLAG_SINGLE_PROCESS | PLAT_FLAG_APP_EXIT;
cc_bool Platform_ReadonlyFilesystem;
#include "../_PlatformBase.h"

// extern unsigned char DEV9_irx[];
// extern unsigned int  size_DEV9_irx;
#define STRINGIFY(a) #a
#define SifLoadBuffer(name) \
	extern unsigned char name[]; \
	extern unsigned int  size_ ## name; \
	ret = SifExecModuleBuffer(name, size_ ## name, 0, NULL, NULL); \
    if (ret < 0) Platform_Log1("SifExecModuleBuffer " STRINGIFY(name) " failed: %i", &ret);


/*########################################################################################################################*
*-----------------------------------------------------Main entrypoint-----------------------------------------------------*
*#########################################################################################################################*/
#include "../main_impl.h"
static void SetupIOPCore(void);
static void GetBootDevice(int argc, char** argv);

int main(int argc, char** argv) {
	GetBootDevice(argc, argv);
	SetupIOPCore();

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
void Platform_Log(const char* msg, int len) {
	char tmp[2048 + 1];
	len = min(len, 2048);
	Mem_Copy(tmp, msg, len); tmp[len] = '\0';
	_print("%s\n", tmp);
}

// https://stackoverflow.com/a/42340213
static CC_INLINE int UnBCD(unsigned char bcd) {
    return bcd - 6 * (bcd >> 4);
}

#define JST_OFFSET -9 * 60 // UTC+9 -> UTC
static time_t CurrentUnixTime(void) {
    sceCdCLOCK raw;
    struct tm tim;
    sceCdReadClock(&raw);

    tim.tm_sec  = UnBCD(raw.second);
    tim.tm_min  = UnBCD(raw.minute);
    tim.tm_hour = UnBCD(raw.hour);
    tim.tm_mday = UnBCD(raw.day);
	// & 0x1F, since a user had issues with upper 3 bits being set to 1
    tim.tm_mon  = UnBCD(raw.month & 0x1F) - 1;
    tim.tm_year = UnBCD(raw.year) + 100;

	// mktime will normalise the time anyways
    tim.tm_min += JST_OFFSET;
    return mktime(&tim);
}

TimeMS DateTime_CurrentUTC(void) {
	time_t rtc_sec = CurrentUnixTime();
	return (cc_uint64)rtc_sec + UNIX_EPOCH_SECONDS;
}

void DateTime_CurrentLocal(struct cc_datetime* t) {
	time_t rtc_sec = CurrentUnixTime();
	struct tm loc_time;
	localtime_r(&rtc_sec, &loc_time);

	t->year   = loc_time.tm_year + 1900;
	t->month  = loc_time.tm_mon  + 1;
	t->day    = loc_time.tm_mday;
	t->hour   = loc_time.tm_hour;
	t->minute = loc_time.tm_min;
	t->second = loc_time.tm_sec;
}


/*########################################################################################################################*
*--------------------------------------------------------Stopwatch--------------------------------------------------------*
*#########################################################################################################################*/
#define US_PER_SEC 1000000ULL

cc_uint64 Stopwatch_Measure(void) { 
	return GetTimerSystemTime();
}

cc_uint64 Stopwatch_ElapsedMicroseconds(cc_uint64 beg, cc_uint64 end) {
	if (end < beg) return 0;
	return (end - beg) * US_PER_SEC / kBUSCLK;
}


/*########################################################################################################################*
*-----------------------------------------------------Directory/File------------------------------------------------------*
*#########################################################################################################################*/
static char root_buffer[32];
static cc_string root_path = { root_buffer, 0, sizeof(root_buffer) };
static char boot_device[16];

// Different IOP filesystem drivers report the same error using different codes
//  (e.g. MCMAN uses -4 for 'no such entry', while FATFS/HostFS use -ENOENT)
static cc_result MapFioError(int res) {
	if (res >= 0) return 0;

	if (res == -ENOENT || res == -4) return ReturnCode_FileNotFound;
	if (res == -EEXIST || res == -8) return ReturnCode_DirectoryExists;
	return res;
}

void Platform_EncodePath(cc_filepath* dst, const cc_string* path) {
	static const cc_string cdrom = String_FromConst("cdrom0:");
	char* str = dst->buffer;

	// Paths that already start with a device (e.g. "cdrom0:") aren't relative to the root directory
	if (String_IndexOf(path, ':') == -1) {
		Mem_Copy(str, root_path.buffer, root_path.length);
		str += root_path.length;
	}
	String_EncodeUtf8(str, path);

	// Files on the disc are named with ISO 9660 version numbers (e.g. MUSIC1.WAV;1)
	if (String_CaselessStarts(path, &cdrom)) {
		str += String_Length(str);
		str[0] = ';'; str[1] = '1'; str[2] = '\0';
	}
}

static cc_bool BootedFrom(const char* device);

const char* Platform_BundledMusicDir(void) {
	// Music is stored on the disc, so isn't available when e.g. booted from USB
	return BootedFrom("cdrom") ? "cdrom0:\\MUSIC\\" : NULL;
}

void Platform_DecodePath(cc_string* dst, const cc_filepath* path) {
	const char* str = path->buffer;
	String_AppendUtf8(dst, str, String_Length(str));
}

void Directory_GetCachePath(cc_string* path) { }

cc_result Directory_Create2(const cc_filepath* path) {
	int res = fioMkdir(path->buffer), fd;
	if (res >= 0) return 0;

	// MCMAN reports creating an existing directory as 'no such entry' (-4), so check
	//  whether the directory actually exists to tell that apart from real failures
	fd = fioDopen(path->buffer);
	if (fd >= 0) { fioDclose(fd); return ReturnCode_DirectoryExists; }
	return MapFioError(res);
}

int File_Exists(const cc_filepath* path) {
	// NOTE: Can't use fioGetstat, as with memory cards it succeeds even when the file doesn't
	//  exist (and leaves the result from the previous call in the stat buffer)
	int fd = fioOpen(path->buffer, FIO_O_RDONLY);
	if (fd < 0) return false;

	fioClose(fd);
	return true;
}

int Directory_GetFreeSpaceKB(void) {
	static const cc_string mc0 = String_FromConst("mc0:");
	static const cc_string mc1 = String_FromConst("mc1:");
	int port, type = 0, free = 0, format = 0, ret = -1;

	// USB drives and PCSX2's host folder have plenty of room
	if (String_CaselessStarts(&root_path, &mc0)) {
		port = 0;
	} else if (String_CaselessStarts(&root_path, &mc1)) {
		port = 1;
	} else {
		return -1;
	}

	// Memory cards use 1 KB clusters
	mcGetInfo(port, 0, &type, &free, &format);
	mcSync(0, NULL, &ret);
	return (ret == 0 || ret == -1) ? free : 0;
}

// Different IOP filesystem drivers return directory entries in different layouts:
//  - io_dirent_t, with the name at offset 40
//  - iox_dirent_t (e.g. iomanX based drivers), with the name at offset 64
//  - PCSX2's host: filesystem, which returns the name at offset 32 (and no usable mode)
#define DIRENT_NAME_IO   40
#define DIRENT_NAME_IOX  64
#define DIRENT_NAME_HOST 32
union DirEntry { io_dirent_t io; cc_uint8 raw[512]; };

static cc_bool IsEntryName(const cc_uint8* src) {
	int i;
	if (!src[0] || src[-1]) return false; // must be a NUL terminated string on its own

	for (i = 0; i < 256 && src[i]; i++)
	{
		if (src[i] < ' ') return false;
	}
	return i < 256;
}

static char* GetEntryName(union DirEntry* entry, int* offset) {
	static const int offsets[] = { DIRENT_NAME_IO, DIRENT_NAME_IOX, DIRENT_NAME_HOST };
	int i;

	for (i = 0; i < Array_Elems(offsets); i++)
	{
		*offset = offsets[i];
		if (IsEntryName(entry->raw + *offset)) return (char*)entry->raw + *offset;
	}

	// Unknown layout, so fallback to first non-empty character after io_dirent_t name offset
	for (i = DIRENT_NAME_IO; i < DIRENT_NAME_IO + 256; i++)
	{
		*offset = i;
		if (entry->raw[i]) return (char*)entry->raw + i;
	}
	return NULL;
}

static int IsEntryDirectory(union DirEntry* entry, int offset, const cc_string* path) {
	cc_filepath str;
	io_stat_t sb;
	if (offset != DIRENT_NAME_HOST) return FIO_SO_ISDIR(entry->io.stat.mode);

	// Mode isn't returned in this layout, so have to query it separately
	Platform_EncodePath(&str, path);
	return fioGetstat(str.buffer, &sb) >= 0 && FIO_SO_ISDIR(sb.mode);
}

cc_result Directory_Enum(const cc_string* dirPath, void* obj, Directory_EnumCallback callback) {
	cc_string path; char pathBuffer[FILENAME_SIZE];
	cc_filepath str;
	union DirEntry entry;
	int fd, res, offset;

	Platform_EncodePath(&str, dirPath);
	fd = fioDopen(str.buffer);
	if (fd < 0) return MapFioError(fd);
	
	res = 0;
	String_InitArray(path, pathBuffer);

	for (;;) {
		Mem_Set(&entry, 0, sizeof(entry));
		if ((res = fioDread(fd, &entry.io)) <= 0) break;

		path.length = 0;
		String_Format1(&path, "%s/", dirPath);

		char* src = GetEntryName(&entry, &offset);
		if (!src) continue;

		// ignore . and .. entry
		if (src[0] == '.' && src[1] == '\0') continue;
		if (src[0] == '.' && src[1] == '.' && src[2] == '\0') continue;

		int len = String_Length(src);
		String_AppendUtf8(&path, src, len);

		int is_dir = IsEntryDirectory(&entry, offset, &path);
		callback(&path, obj, is_dir);
	}

	fioDclose(fd);
	return MapFioError(res);
}

static cc_result File_Do(cc_file* file, const char* path, int mode) {
	int res = fioOpen(path, mode);
	*file   = res;
	return MapFioError(res);
}

cc_result File_Open(cc_file* file, const cc_filepath* path) {
	cc_result res = File_Do(file, path->buffer, FIO_O_RDONLY);
	io_stat_t sb;

	// Some drivers (e.g. PCSX2's HostFS) return generic errors such as -EIO
	//  for missing files, so double check whether the file actually exists
	if (res && res != ReturnCode_FileNotFound && fioGetstat(path->buffer, &sb) < 0)
		return ReturnCode_FileNotFound;
	return res;
}
cc_result File_Create(cc_file* file, const cc_filepath* path) {
	return File_Do(file, path->buffer, FIO_O_RDWR | FIO_O_CREAT | FIO_O_TRUNC);
}
cc_result File_OpenOrCreate(cc_file* file, const cc_filepath* path) {
	return File_Do(file, path->buffer, FIO_O_RDWR | FIO_O_CREAT);
}

cc_result File_Read(cc_file file, void* data, cc_uint32 count, cc_uint32* bytesRead) {
	int res    = fioRead(file, data, count);
	*bytesRead = res;
	return res < 0 ? res : 0;
}

cc_result File_Write(cc_file file, const void* data, cc_uint32 count, cc_uint32* bytesWrote) {
	int res     = fioWrite(file, data, count);
	*bytesWrote = res;
	return res < 0 ? res : 0;
}

cc_result File_Close(cc_file file) {
	int res = fioClose(file);
	return res < 0 ? res : 0;
}

cc_result File_Seek(cc_file file, int offset, int seekType) {
	static cc_uint8 modes[3] = { SEEK_SET, SEEK_CUR, SEEK_END };
	
	int res = fioLseek(file, offset, modes[seekType]);
	return res < 0 ? res : 0;
}

cc_result File_Position(cc_file file, cc_uint32* pos) {
	int res = fioLseek(file, 0, SEEK_CUR);
	*pos    = res;
	return res < 0 ? res : 0;
}

cc_result File_Length(cc_file file, cc_uint32* len) {
	int cur_pos = fioLseek(file, 0, SEEK_CUR);
	if (cur_pos < 0) return cur_pos; // error occurred
	
	// get end and then restore position
	int res = fioLseek(file, 0, SEEK_END);
	fioLseek(file, cur_pos, SEEK_SET);
	
	*len    = res;
	return res < 0 ? res : 0;
}


/*########################################################################################################################*
*--------------------------------------------------------Threading--------------------------------------------------------*
*#########################################################################################################################*/
void Thread_Sleep(cc_uint32 milliseconds) {
	DelayThread(milliseconds * 1000);
}

static int ExecThread(void* param) {
	((Thread_StartFunc)param)(); 
	
	int thdID = GetThreadId();
	ee_thread_status_t info;
	
	int res = ReferThreadStatus(thdID, &info);
	if (res > 0 && info.stack) Mem_Free(info.stack); // TODO is it okay to free stack of running thread ????
	
	return 0; // TODO detach ?
}

// The network stack (ps2ip/netman) threads run at priorities 86-89. Since the EE kernel never
//  timeslices, the main thread must run at a lower priority (higher value) than them,
//  otherwise they never get to run while the main thread is busy rendering
#define MAIN_THREAD_PRIO 100

void Thread_Run(void** handle, Thread_StartFunc func, int stackSize, const char* name) {
	ee_thread_t thread = { 0 };
	thread.func        = ExecThread;
	thread.stack       = Mem_Alloc(stackSize, 1, "Thread stack");
	thread.stack_size  = stackSize;
	thread.gp_reg      = &_gp;
	// Main thread yields to same priority threads every frame (see Window_ProcessEvents)
	thread.initial_priority = MAIN_THREAD_PRIO;
	
	int thdID = CreateThread(&thread);
	if (thdID < 0) Process_Abort2(thdID, "Creating thread");
	*handle = (void*)thdID;
	
	int res = StartThread(thdID, (void*)func);
	if (res < 0) Process_Abort2(res, "Running thread");
}

void Thread_Detach(void* handle) {
	// TODO do something
}

void Thread_Join(void* handle) {
	int thdID = (int)handle;
	ee_thread_status_t info;
	
	for (;;)
	{
		int res = ReferThreadStatus(thdID, &info);
		if (res) Process_Abort("Checking thread status");
		
		if (info.status == THS_DORMANT) break;
		Thread_Sleep(10); // TODO better solution
	}
}


/*########################################################################################################################*
*-----------------------------------------------------Synchronisation-----------------------------------------------------*
*#########################################################################################################################*/
void* Mutex_Create(const char* name) {
	ee_sema_t sema  = { 0 };
	sema.init_count = 1;
	sema.max_count  = 1;
	
	int semID = CreateSema(&sema);
	if (semID < 0) Process_Abort2(semID, "Creating mutex");
	return (void*)semID;
}

void Mutex_Free(void* handle) {
	int semID = (int)handle;
	int res   = DeleteSema(semID);
	
	if (res) Process_Abort2(res, "Destroying mutex");
}

void Mutex_Lock(void* handle) {
	int semID = (int)handle;
	int res   = WaitSema(semID);
	
	if (res < 0) Process_Abort2(res, "Locking mutex");
}

void Mutex_Unlock(void* handle) {
	int semID = (int)handle;
	int res   = SignalSema(semID);
	
	if (res < 0) Process_Abort2(res, "Unlocking mutex");
}

void* Waitable_Create(const char* name) {
	ee_sema_t sema  = { 0 };
	sema.init_count = 0;
	sema.max_count  = 1;
	
	int semID = CreateSema(&sema);
	if (semID < 0) Process_Abort2(semID, "Creating waitable");
	return (void*)semID;
}

void Waitable_Free(void* handle) {
	int semID = (int)handle;
	int res   = DeleteSema(semID);
	
	if (res < 0) Process_Abort2(res, "Destroying waitable");
}

void Waitable_Signal(void* handle) {
	int semID = (int)handle;
	int res   = SignalSema(semID);
	
	if (res < 0) Process_Abort2(res, "Signalling event");
}

void Waitable_Wait(void* handle) {
	int semID = (int)handle;
	int res   = WaitSema(semID);
	
	if (res < 0) Process_Abort2(res, "Signalling event");
}

void Waitable_WaitFor(void* handle, cc_uint32 milliseconds) {
	int semID = (int)handle;
	cc_uint32 waited;

	// Poll in small steps, as there's no simple wait with timeout
	for (waited = 0; waited < milliseconds; waited += 10)
	{
		if (PollSema(semID) >= 0) return;
		Thread_Sleep(10);
	}
}


/*########################################################################################################################*
*-------------------------------------------------------Networking--------------------------------------------------------*
*#########################################################################################################################*/
int	ps2ip_getconfig(char* netif_name,t_ip_info* ip_info);
int ps2ip_setconfig(const t_ip_info* ip_info);
static cc_bool net_supported;

// https://github.com/ps2dev/ps2sdk/blob/master/NETMAN.txt
// https://github.com/ps2dev/ps2sdk/blob/master/ee/network/tcpip/samples/tcpip_dhcp/ps2ip.c
static void Net_StatusCheckCb(s32 alarm_id, u16 time, void *common) {
	int threadID = *(int*)common;
	iWakeupThread(threadID);
}

typedef int (*WaitCheckFunc)(void);
static int WaitUntilReady(WaitCheckFunc checkingFunction, int maxRetries) {
	int threadID = GetThreadId();

	for (int retries = 0; checkingFunction() == 0; retries++)
	{
		if (retries >= maxRetries) return -1;

		// Sleep for 500ms
		SetAlarm(500 * 16, &Net_StatusCheckCb, &threadID);
		SleepThread();
	}
	return 0;
}

static int Net_CheckIFLinkStatus(void) {
	int status = NetManIoctl(NETMAN_NETIF_IOCTL_GET_LINK_STATUS, NULL, 0, NULL, 0);
	return status == NETMAN_NETIF_ETH_LINK_STATE_UP;
}

static int Net_CheckDHCPStatus(void) {
	t_ip_info ip_info;
	// NOTE: ps2ip_getconfig returns 0 on failure
	if (ps2ip_getconfig("sm0", &ip_info) <= 0) return -1;

	if (ip_info.dhcp_enabled) {
		return ip_info.dhcp_status == DHCP_STATE_BOUND || ip_info.dhcp_status == DHCP_STATE_OFF;
	}
	return -1;
}

static int Net_EnableDHCP(void) {
	t_ip_info ip_info;
	// SMAP is registered as the "sm0" device to the TCP/IP stack.
	if (ps2ip_getconfig("sm0", &ip_info) <= 0) return -1;

	if (!ip_info.dhcp_enabled) {
		ip_info.dhcp_enabled = 1;
		return ps2ip_setconfig(&ip_info);
	}
	return 1;
}

static void Networking_Setup(void) {
	NetManInit();

	struct ip4_addr IP  = { 0 }, NM = { 0 }, GW = { 0 };
	ps2ipInit(&IP, &NM, &GW);
	net_supported = true;

	// DHCP lease is acquired in the background (see Networking_EnsureReady)
	int res = Net_EnableDHCP();
	if (res < 0) Platform_Log1("Error %i enabling DHCP", &res);
}

// Acquiring a DHCP lease can take 5+ seconds (due to lwIP probing for
//  IP address conflicts), so rather than delaying startup by that long,
//  only wait for the network to be ready when it is first actually used
static cc_bool net_ready;

static cc_result Networking_EnsureReady(void) {
	if (!net_supported) return ERR_NO_NETWORKING;
	if (net_ready)      return 0;

	Platform_LogConst("Waiting for net link connection...");
	if (WaitUntilReady(Net_CheckIFLinkStatus, 10) != 0) {
		Platform_LogConst("Failed to establish network link");
		return ERR_NO_NETWORKING;
	}

	Platform_LogConst("Waiting for DHCP lease...");
	if (WaitUntilReady(Net_CheckDHCPStatus, 30) != 0) {
		Platform_LogConst("Failed to acquire DHCP lease");
		return ERR_NO_NETWORKING;
	}

	Platform_LogConst("Network setup done");
	net_ready = true;
	return 0;
}

cc_bool Socket_NetworkAvailable(void) {
	if (!net_supported) return false; // no network adapter
	if (net_ready)      return true;

	// Only wait for the link to come up (i.e. cable plugged in), as also waiting
	//  for a DHCP lease would delay startup by 5+ more seconds
	// NOTE: Link takes ~3 seconds to come up, so use same timeout as Networking_EnsureReady
	return WaitUntilReady(Net_CheckIFLinkStatus, 10) == 0;
}

// Returns whether a network adapter is available
static cc_bool Networking_LoadIOPModules(void) {
	int ret;

	SifLoadBuffer(ps2dev9_irx);
	if (ret < 0) return false;
	SifLoadBuffer(netman_irx);
	if (ret < 0) return false;
	// SMAP fails to load when there's no network adapter
	SifLoadBuffer(smap_irx);
	return ret >= 0;
}

/*########################################################################################################################*
*---------------------------------------------------------Socket----------------------------------------------------------*
*#########################################################################################################################*/
int lwip_shutdown(int s, int how);
int lwip_getsockopt(int s, int level, int optname, void *optval, socklen_t *optlen);
int lwip_setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen);
int lwip_close(int s);
int lwip_connect(int s, const struct sockaddr *name, socklen_t namelen);
int lwip_recv(int s, void *mem, size_t len, int flags);
int lwip_send(int s, const void *dataptr, size_t size, int flags);
int lwip_socket(int domain, int type, int protocol);
int lwip_select(int maxfdp1, fd_set *readset, fd_set *writeset, fd_set *exceptset, struct timeval *timeout);
int lwip_ioctl(int s, long cmd, void *argp);

int netconn_gethostbyname(const char *name, ip4_addr_t *addr);

cc_bool SockAddr_ToString(const cc_sockaddr* addr, cc_string* dst) {
	struct sockaddr_in* addr4 = (struct sockaddr_in*)addr->data;

	if (addr4->sin_family == AF_INET) 
		return IPv4_ToString(&addr4->sin_addr, &addr4->sin_port, dst);
	return false;
}

static cc_bool ParseIPv4(const cc_string* ip, int port, cc_sockaddr* dst) {
	struct sockaddr_in* addr4 = (struct sockaddr_in*)dst->data;
	cc_uint32 ip_addr = 0;
	if (!ParseIPv4Address(ip, &ip_addr)) return false;

	addr4->sin_addr.s_addr = ip_addr;
	addr4->sin_family      = AF_INET;
	addr4->sin_port        = SockAddr_EncodePort(port);
		
	dst->size = sizeof(*addr4);
	return true;
}

static cc_bool ParseIPv6(const char* ip, int port, cc_sockaddr* dst) {
	return false;
}

static cc_result ParseHost(const char* host, int port, cc_sockaddr* addrs, int* numValidAddrs) {
	struct sockaddr_in* addr4 = (struct sockaddr_in*)addrs[0].data;
	ip4_addr_t addr;
	cc_result res = Networking_EnsureReady();
	if (res) return res;

	res = netconn_gethostbyname(host, &addr);
	//if (res == -NO_DATA) return SOCK_ERR_UNKNOWN_HOST;
	if (res) return res;

    addr4->sin_addr.s_addr = addr.addr;
	addr4->sin_family      = AF_INET;
	addr4->sin_port        = SockAddr_EncodePort(port);
		
	addrs[0].size  = sizeof(*addr4);
	*numValidAddrs = 1;
	return 0;
}

static cc_result GetSocketError(cc_socket s) {
	socklen_t resultSize = sizeof(socklen_t);
	cc_result res = 0;
	lwip_getsockopt(s, SOL_SOCKET, SO_ERROR, &res, &resultSize);
	return res;
}

cc_result Socket_Create(cc_socket* s, cc_sockaddr* addr) {
	struct sockaddr* raw = (struct sockaddr*)addr->data;
	cc_result res = Networking_EnsureReady();
	if (res) { *s = -1; return res; }

	*s = lwip_socket(raw->sa_family, SOCK_STREAM, 0);
	if (*s < 0) return *s;

	return 0;
}

cc_result Socket_SetNonBlocking(cc_socket s, cc_bool nonblocking) {
	int mode = nonblocking ? 1 : 0;
	int res  = lwip_ioctl(s, FIONBIO, &mode);
	return res == -1 ? GetSocketError(s) : 0;
}

void Socket_Close(cc_socket s) {
	if (s < 0) return;
	lwip_shutdown(s, SHUT_RDWR);
	lwip_close(s);
}

cc_result Socket_Connect(cc_socket s, const void* addr, int addrSize) {
	int res = lwip_connect(s, (struct sockaddr*)addr, addrSize);
	return res == -1 ? GetSocketError(s) : 0;
}

cc_result Socket_Read(cc_socket s, cc_uint8* data, cc_uint32 count, cc_uint32* modified) {
	//Platform_Log1("PREPARE TO READ: %i", &count);
	int recvCount = lwip_recv(s, data, count, 0);
	//Platform_Log1(" .. read %i", &recvCount);
	if (recvCount != -1) { *modified = recvCount; return 0; }
	
	/* NOTE: SO_ERROR is 0 for EWOULDBLOCK, so it must be read from errno instead */
	*modified = 0; return errno;
}

cc_result Socket_Write(cc_socket s, const cc_uint8* data, cc_uint32 count, cc_uint32* modified) {
	//Platform_Log1("PREPARE TO WRITE: %i", &count);
	int sentCount = lwip_send(s, data, count, 0);
	//Platform_Log1(" .. wrote %i", &sentCount);
	if (sentCount != -1) { *modified = sentCount; return 0; }
	
	*modified = 0; return errno;
}

cc_result Socket_Poll(cc_socket s, int timeoutMS, int mode, cc_bool* success) {
	fd_set read_set, write_set, error_set;
	int selectCount;
	struct timeval time = {
		timeoutMS / 1000,          /* seconds */
		(timeoutMS % 1000) * 1000, /* microseconds */
	};

	FD_ZERO(&read_set);
	FD_SET(s, &read_set);
	FD_ZERO(&write_set);
	FD_SET(s, &write_set);
	FD_ZERO(&error_set);
	FD_SET(s, &error_set);

	selectCount = lwip_select(s + 1, &read_set, &write_set, &error_set, &time);

	//Platform_Log4("SELECT %i = %h / %h / %h", &selectCount, &read_set, &write_set, &error_set);
	if (selectCount == -1) { *success = false; return errno; }
	*success = FD_ISSET(s, &write_set) != 0; return 0;
}


/*########################################################################################################################*
*----------------------------------------------------USB mass storage-----------------------------------------------------*
*#########################################################################################################################*/
static void USBStorage_LoadIOPModules(void) {   
    int ret;
    // TODO: Seems that
    // BDM, BDMFS_FATFS, USBMASS_BD - newer ?
    // USBHDFSD - older ?
    
	SifLoadBuffer(usbd_irx);  
	//SifLoadBuffer(USBHDFSD_irx);
    
	SifLoadBuffer(bdm_irx);
	SifLoadBuffer(bdmfs_fatfs_irx);
    
	SifLoadBuffer(usbmass_bd_irx);
	SifLoadBuffer(ps2mouse_irx);
	SifLoadBuffer(ps2kbd_irx);
}

// USB devices are detected asynchronously by BDM after the modules are loaded,
//  so "mass:/" may not be accessible immediately after startup
static cc_bool USBStorage_WaitUntilDeviceReady(int retries) {
	for (int retry = 0; retry < retries; retry++)
	{
		int fd = fioDopen("mass:/");
		if (fd >= 0) { fioDclose(fd); return true; }

		Thread_Sleep(100);
	}
	return false;
}


/*########################################################################################################################*
*-------------------------------------------------------Filesystem--------------------------------------------------------*
*#########################################################################################################################*/
static void GetBootDevice(int argc, char** argv) {
	if (argc < 1 || !argv[0]) return;
	const char* path = argv[0];

	// e.g. "mass:/APPS/ClassiCube.elf" -> "mass"
	for (int i = 0; i < sizeof(boot_device) - 1 && path[i]; i++)
	{
		if (path[i] != ':') continue;

		Mem_Copy(boot_device, path, i);
		boot_device[i] = '\0';
		return;
	}
}

// MCMAN rejects accesses to a card until it has been detected using mcGetInfo
static cc_bool MemoryCard_IsReady(int port) {
	static cc_bool inited;
	int type = 0, free = 0, format = 0, ret = -1;

	if (!inited) {
		ret = mcInit(MC_TYPE_MC);
		if (ret < 0) { Platform_Log1("mcInit failed: %i", &ret); return false; }
		inited = true;
	}

	mcGetInfo(port, 0, &type, &free, &format);
	mcSync(0, NULL, &ret);

	// 0 = same card as last time, -1 = a different formatted card was inserted
	if (ret != 0 && ret != -1) return false;
	return type == MC_TYPE_PS2 && format;
}

// e.g. both "mass" and "mass0" count as booting from "mass"
static cc_bool BootedFrom(const char* device) {
	cc_string boot = String_FromReadonly(boot_device);
	cc_string dev  = String_FromReadonly(device);
	return String_CaselessStarts(&boot, &dev);
}

static cc_bool TryUseRoot(const char* root) {
	cc_filepath* path = FILEPATH_RAW(root);
	cc_result res     = Directory_Create2(path);
	int fd;

	// Make sure the directory can actually be accessed
	// NOTE: Creating can 'fail' just because it already exists, as e.g. MCMAN
	//  returns -4 instead of an 'already exists' error, so always check this
	fd = fioDopen(root);
	if (fd < 0) {
		Platform_Log3("Can't use %c as root directory: %e (open %i)", root, &res, &fd);
		return false;
	}
	fioDclose(fd);

	root_path.length = 0;
	String_AppendConst(&root_path, root);
	String_Append(&root_path, '/');
	Platform_Log1("Using %s as root directory", &root_path);
	return true;
}

static void InitFilesystem(void) {
	Platform_Log1("Boot device: %c", boot_device);
	// Launched through PCSX2 or ps2link, so store data next to the executable
	if (BootedFrom("host") && TryUseRoot("host:ClassiCube")) return;

	// Wait longer when launched from USB, since the device is known to exist
	if (USBStorage_WaitUntilDeviceReady(BootedFrom("mass") ? 30 : 10)
		&& TryUseRoot("mass:/ClassiCube")) return;

	if (MemoryCard_IsReady(0) && TryUseRoot("mc0:/ClassiCube")) return;
	if (MemoryCard_IsReady(1) && TryUseRoot("mc1:/ClassiCube")) return;

	Platform_ReadonlyFilesystem = true;
	String_AppendConst(&root_path, "mass:/ClassiCube/");
	Window_ShowDialog("Data persistence warning",
		"No USB drive or memory card found\nNote that settings and maps cannot be saved");
}


/*########################################################################################################################*
*--------------------------------------------------------Platform---------------------------------------------------------*
*#########################################################################################################################*/
// Note that resetting IOP does mean can't debug through ps2client
// 	https://github.com/libsdl-org/SDL/commit/d355ea9981358a1df335b1f7485ce94768bbf255
// 	https://github.com/libsdl-org/SDL/pull/6022
static void ResetIOP(void) { // reboots the IOP
	SifInitRpc(0);
	while (!SifIopReset("", 0)) { }
	while (!SifIopSync())       { }
}

static void LoadIOPModules(void) {
	int ret;
	
	// file I/O module
	ret = SifLoadModule("rom0:FILEIO",  0, NULL);
    if (ret < 0) Platform_Log1("SifLoadModule FILEIO failed: %i", &ret);
    sbv_patch_fileio();
	
	// serial I/O module (needed for memory card & input pad modules)
	ret = SifLoadModule("rom0:SIO2MAN", 0, NULL);
    if (ret < 0) Platform_Log1("SifLoadModule SIO2MAN failed: %i", &ret);
	
	
	// memory card module
	ret = SifLoadModule("rom0:MCMAN",   0, NULL);
    if (ret < 0) Platform_Log1("SifLoadModule MCMAN failed: %i", &ret);
	
	// memory card server module
	ret = SifLoadModule("rom0:MCSERV",  0, NULL);
    if (ret < 0) Platform_Log1("SifLoadModule MCSERV failed: %i", &ret);
    
    
    // Input pad module
    ret = SifLoadModule("rom0:PADMAN",  0, NULL);
    if (ret < 0) Platform_Log1("SifLoadModule PADMAN failed: %i", &ret);
}

static void SetupIOPCore(void) {
	//InitDebug();
	ResetIOP();
	SifInitRpc(0);
	SifLoadFileInit();
	SifInitIopHeap();
	sbv_patch_enable_lmb(); // Allows loading IRX modules from a buffer in EE RAM

	LoadIOPModules();
}

void Platform_Init(void) {
	ChangeThreadPriority(GetThreadId(), MAIN_THREAD_PRIO);
	USBStorage_LoadIOPModules();
	InitFilesystem();

	if (Networking_LoadIOPModules()) {
		Networking_Setup();
	} else {
		Platform_LogConst("No network adapter found");
	}
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

cc_result Platform_SetDefaultCurrentDirectory(void) { return 0; }

void CPU_FlushDataCache(void* start, int length) {
	SyncDCache(start, start + length);
}


/*########################################################################################################################*
*-------------------------------------------------------Encryption--------------------------------------------------------*
*#########################################################################################################################*/
#define MACHINE_KEY "PS2_PS2_PS2_PS2_"

static cc_result GetMachineID(cc_uint32* key) {
	Mem_Copy(key, MACHINE_KEY, sizeof(MACHINE_KEY) - 1);
	return 0;
}

cc_result Platform_GetEntropy(void* data, int len) {
	return ERR_NOT_SUPPORTED;
}

