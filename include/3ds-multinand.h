#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <winioctl.h>
#include "resource.h"

//#define DEBUG_BUILD

#define SECTOR_SIZE			512
#define MEDIA_UNIT_SIZE		SECTOR_SIZE
#define NAND_BUF_SIZE		(SECTOR_SIZE * 128)							// 64 KiB

#define O3DS_TOSHIBA_NAND	0x3AF00000									// 943 MiB
#define O3DS_SAMSUNG_NAND	0x3BA00000									// 954 MiB

#define N3DS_SAMSUNG_NAND	0x4D800000									// 1240 MiB
#define N3DS_UNKNOWN_NAND	0x74800000									// 1864 MiB
#define N3DS_TOSHIBA_NAND	0x76000000									// 1888 MiB

#define round_up(x,y)		((x) + (((y) - ((x) % (y))) % (y)))			// Aligns 'x' bytes to a 'y' bytes boundary
#define round4MB(x)			round_up((x), 4 * 1024 * 1024)				// 4 MB alignment used by EmuNAND9 Tool

#define O3DS_LEGACY_FAT		0x40000000									// FILE_BEGIN + 1 GiB. Used by Gateway
#define O3DS_DEFAULT_FAT	round4MB(SECTOR_SIZE + O3DS_SAMSUNG_NAND)	// FILE_BEGIN + 512 bytes + 954 MiB + 4 MB alignment
#define O3DS_MINIMUM_FAT	round4MB(SECTOR_SIZE + O3DS_TOSHIBA_NAND)	// FILE_BEGIN + 512 bytes + 943 MiB + 4 MB alignment

#define N3DS_LEGACY_FAT		0x80000000									// FILE_BEGIN + 2 GiB. Used by Gateway
#define N3DS_DEFAULT_FAT	round4MB(SECTOR_SIZE + N3DS_TOSHIBA_NAND)	// FILE_BEGIN + 512 bytes + 1888 MiB + 4 MB alignment
#define N3DS_MINIMUM_FAT	round4MB(SECTOR_SIZE + N3DS_SAMSUNG_NAND)	// FILE_BEGIN + 512 bytes + 1240 MiB + 4 MB alignment

#define NCSD_MAGIC			0x4E435344									// "NCSD"
#define DUMMY_DATA			0x0D0A										// Used to generate the 512-bytes dummy header/footer
#define FAT32_SIGNATURE		0x41615252									// Byteswapped "RRaA"
#define PARTITION_FAT32_LBA	PARTITION_FAT32_XINT13						// 0x0C - Set by the Launcher.dat executable during the EmuNAND format
#define PARTITION_FAT16B	PARTITION_HUGE								// 0x06
#define GIBIBYTE			(1024 * 1024 * 1024)						// Used with the ParseDrives procedure

#define NAME_LENGTH			32											// Null-character terminated string

#define ARRAYSIZE(x)		((sizeof((x))) / (sizeof((x)[0])))			// Returns the number of elements in an array
#define NAND_NUM_STR(x)		((x) == 1 ? L"st" : ((x) == 2 ? L"nd" : ((x) == 3 ? L"rd" : L"th")))
#define NAND_TYPE_STR(x)	(((x) == O3DS_TOSHIBA_NAND || (x) == (O3DS_TOSHIBA_NAND + SECTOR_SIZE) || (x) == (N3DS_TOSHIBA_NAND)) ? L"Toshiba" : (((x) == O3DS_SAMSUNG_NAND || (x) == (O3DS_SAMSUNG_NAND + SECTOR_SIZE) || (x) == N3DS_SAMSUNG_NAND) ? L"Samsung" : L"**Unknown**"))
#define FAT_LAYOUT_STR(x)	(((x) == O3DS_LEGACY_FAT || (x) == N3DS_LEGACY_FAT) ? L"Legacy" : (((x) == O3DS_DEFAULT_FAT || (x) == N3DS_DEFAULT_FAT) ? L"Default" : L"Minimum"))
#define CAPACITY(x,y)		((x) == 1 ? 2 : ((x) == 2 ? 4 : ((x) == 3 ? (!(y) ? 4 : 8) : 8)))

/* Macros taken from Rufus source code */
#define DRIVE_ACCESS_TIMEOUT	15000	// How long we should retry drive access (in ms)
#define DRIVE_ACCESS_RETRIES	60		// How many times we should retry

#define PTR_HIGH(x)			((int32_t)((x) >> 32))
#define PTR_LOW(x)			((int32_t)(x))
#define PTR_FULL(x,y)		(((int64_t)(x) << 32) | (y))
#define bswap_16(a)			((((a) << 8) & 0xff00) | (((a) >> 8) & 0xff))
#define bswap_32(a)			((((a) << 24) & 0xff000000) | (((a) << 8) & 0xff0000) | (((a) >> 8) & 0xff00) | (((a) >> 24) & 0xff))

typedef struct {
	wchar_t drive_str[50];
	wchar_t drive_letter[4];
	uint32_t drive_num;
	int64_t drive_sz;
	int64_t fat_offset;
	int64_t fat_layout;
	bool n3ds;
	bool n2ds;
	int8_t emunand_cnt;
	uint32_t emunand_sizes[MAX_NAND_NUM];
	bool rednand[MAX_NAND_NUM];
} DRIVE_INFO;

int8_t nandnum;
bool is_input, cfw;
char nand_name[NAME_LENGTH];

int GetTextSize(LPTSTR str);
int64_t set_file_pointer(HANDLE h, int64_t new_ptr, uint32_t method);
char *GetLogicalName(HWND hWndParent, DWORD DriveIndex, BOOL bKeepTrailingBackslash);
void RemoveNAND(HWND hWndParent);
int WriteReadNANDName(HWND hWndParent, bool read);
int ParseDrives(HWND hWndParent, bool check_fixed);
void InjectExtractNAND(wchar_t *fname, HWND hWndParent, bool isFormat);
