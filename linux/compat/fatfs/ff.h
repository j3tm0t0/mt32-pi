//
// fatfs/ff.h - Linux compatibility shim for mt32-pi
//
// Implements the small subset of the FatFs API used by mt32-pi's portable
// code (config, ROM and SoundFont managers) on top of POSIX file I/O.
//
// FatFs volume prefixes are mapped to host directories:
//   "SD:foo"  -> <sd root>/foo
//   "USB:foo" -> <usb root>/foo   (fails if no USB root is configured)
//   "foo"     -> <sd root>/foo    (default volume)
//

#ifndef _fatfs_ff_h
#define _fatfs_ff_h

#include <circle/types.h>
#include <cstdio>
#include <string>

typedef unsigned int UINT;
typedef unsigned char BYTE;
typedef u64 FSIZE_t;

typedef enum
{
	FR_OK = 0,
	FR_DISK_ERR,
	FR_INT_ERR,
	FR_NOT_READY,
	FR_NO_FILE,
	FR_NO_PATH,
	FR_INVALID_NAME,
	FR_DENIED,
	FR_EXIST,
	FR_INVALID_OBJECT,
	FR_WRITE_PROTECTED,
	FR_INVALID_DRIVE,
	FR_NOT_ENABLED,
	FR_NO_FILESYSTEM,
	FR_MKFS_ABORTED,
	FR_TIMEOUT,
	FR_LOCKED,
	FR_NOT_ENOUGH_CORE,
	FR_TOO_MANY_OPEN_FILES,
	FR_INVALID_PARAMETER
} FRESULT;

#define FA_READ 0x01
#define FA_WRITE 0x02
#define FA_OPEN_EXISTING 0x00
#define FA_CREATE_NEW 0x04
#define FA_CREATE_ALWAYS 0x08
#define FA_OPEN_ALWAYS 0x10

#define AM_RDO 0x01
#define AM_HID 0x02
#define AM_SYS 0x04
#define AM_DIR 0x10
#define AM_ARC 0x20

struct FIL
{
	FILE* pFile = nullptr;
	FSIZE_t nSize = 0;
};

// FatFs' DIR clashes with POSIX DIR from <dirent.h>; rename it
#define DIR FF_DIR
struct FF_DIR
{
	void* pDir = nullptr;
	std::string Pattern;
};

struct FILINFO
{
	FSIZE_t fsize;
	BYTE fattrib;
	char fname[256];
};

// Host directory mapping
void ff_set_volume_root(const char* pVolume, const char* pHostPath);
std::string ff_translate_path(const char* pPath);

FRESULT f_open(FIL* fp, const char* path, BYTE mode);
FRESULT f_close(FIL* fp);
FRESULT f_read(FIL* fp, void* buff, UINT btr, UINT* br);
FRESULT f_lseek(FIL* fp, FSIZE_t ofs);
FSIZE_t f_tell(FIL* fp);
inline FSIZE_t f_size(FIL* fp) { return fp->nSize; }

FRESULT f_findfirst(DIR* dp, FILINFO* fno, const char* path, const char* pattern);
FRESULT f_findnext(DIR* dp, FILINFO* fno);
FRESULT f_closedir(DIR* dp);

#endif
