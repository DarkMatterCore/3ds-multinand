#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define SECTOR_SIZE			512
#define MEDIA_UNIT_SIZE		SECTOR_SIZE
#define NAND_BUF_SIZE		(SECTOR_SIZE * 128)					// 64 KiB

#define BOOT_BIN_SECTOR		0x14								// Contains the sector from which the RedNAND will be booted
#define BOOT_BIN_SIZE		0x1D8C								// 7564 bytes

#define O3DS_FS_BASE_SECTOR	0x40000000							// FILE_BEGIN + 1 GiB
#define N3DS_FS_BASE_SECTOR	0x80000000							// FILE_BEGIN + 2 GiB

#define TOSHIBA_NAND		0x3AF00000							// 943 MiB
#define TOSHIBA_REDNAND		(TOSHIBA_NAND + SECTOR_SIZE)		// 943 MiB + 512 bytes
#define SAMSUNG_NAND		0x3BA00000							// 954 MiB
#define SAMSUNG_REDNAND		(SAMSUNG_NAND + SECTOR_SIZE)		// 954 MiB + 512 bytes

#define N3DS_SAMSUNG_NAND	0x4D800000							// 1240 MiB
#define N3DS_UNKNOWN_NAND	0x76000000							// 1888 MiB

#define NCSD_MAGIC			0x4E435344							// "NCSD"
#define DUMMY_DATA			0x0D0A								// Used to generate the 512-bytes dummy header
#define FAT32_SIGNATURE		0x41615252							// "RRaA"
#define PARTITION_FAT32_LBA	0x0C								// Set by the Launcher.dat executable during the EmuNAND format
#define PARTITION_FAT16B	0x06

#define MAX_CHARACTERS(x)	((sizeof((x))) / (sizeof((x)[0])))	// Returns the number of elements in an array
#define NAND_NUM_STR(x)		((x) == 1 ? L"st" : ((x) == 2 ? L"nd" : ((x) == 3 ? L"rd" : L"th")))
#define NAND_TYPE_STR(x)	(((x) == TOSHIBA_NAND || (x) == TOSHIBA_REDNAND) ? L"Toshiba" : (((x) == SAMSUNG_NAND || (x) == SAMSUNG_REDNAND || (x) == N3DS_SAMSUNG_NAND) ? L"Samsung" : L"**Unknown**"))

#define PTR_HIGH(x)			((int32_t)((x) >> 32))
#define PTR_LOW(x)			((int32_t)(x))
#define PTR_FULL(x,y)		(((int64_t)(x) << 32) | (y))
#define bswap_16(a)			((((a) << 8) & 0xff00) | (((a) >> 8) & 0xff))
#define bswap_32(a)			((((a) << 24) & 0xff000000) | (((a) << 8) & 0xff0000) | (((a) >> 8) & 0xff00) | (((a) >> 24) & 0xff))

int8_t nandnum;
bool n3ds, is_input, cfw;

int GetTextSize(LPTSTR str);
int ParseDrives(HWND hWndParent);
void InjectExtractNAND(wchar_t *fname, HWND hWndParent, bool isFormat);
void ModifyBootBin(wchar_t *fname, HWND hWndParent);
