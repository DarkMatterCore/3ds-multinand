// Fat32 formatter version 1.07
// (c) Tom Thornhill 2007,2008,2009
// This software is covered by the GPL. 
// By using this tool, you agree to absolve Ridgecrop of an liabilities for lost data.
// Please backup any data you value before using this tool.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winioctl.h>  // From the Win32 SDK \Mstools\Include, or Visual Studio.Net

#include "3ds-multinand.h"

static wchar_t msg_info[512] = {0};

#pragma pack(push, 1)

typedef struct tagFAT_BOOTSECTOR32
{
	// Common fields.
	BYTE sJmpBoot[3];
	BYTE sOEMName[8];
	WORD wBytsPerSec;
	BYTE bSecPerClus;
	WORD wRsvdSecCnt;
	BYTE bNumFATs;
	WORD wRootEntCnt;
	WORD wTotSec16; // if zero, use dTotSec32 instead
	BYTE bMedia;
	WORD wFATSz16;
	WORD wSecPerTrk;
	WORD wNumHeads;
	DWORD dHiddSec;
	DWORD dTotSec32;
	// Fat 32/16 only
	DWORD dFATSz32;
	WORD wExtFlags;
	WORD wFSVer;
	DWORD dRootClus;
	WORD wFSInfo;
	WORD wBkBootSec;
	BYTE Reserved[12];
	BYTE bDrvNum;
	BYTE Reserved1;
	BYTE bBootSig; // == 0x29 if next three fields are ok
	DWORD dBS_VolID;
	BYTE sVolLab[11];
	BYTE sBS_FilSysType[8];
} FAT_BOOTSECTOR32;

typedef struct {
	DWORD dLeadSig;		 // 0x41615252
	BYTE sReserved1[480];   // zeros
	DWORD dStrucSig;		// 0x61417272
	DWORD dFree_Count;	  // 0xFFFFFFFF
	DWORD dNxt_Free;		// 0xFFFFFFFF
	BYTE sReserved2[12];	// zeros
	DWORD dTrailSig;	 // 0xAA550000
} FAT_FSINFO;

#pragma pack(pop)

DWORD get_volume_id()
{
	SYSTEMTIME s;
	DWORD d;
	WORD lo, hi, tmp;

	GetLocalTime(&s);

	lo = s.wDay + (s.wMonth << 8);
	tmp = (s.wMilliseconds / 10) + (s.wSecond << 8);
	lo += tmp;

	hi = s.wMinute + (s.wHour << 8);
	hi += s.wYear;
	
	d = lo + (hi << 16);
	return d;
}

/*
This is the Microsoft calculation from FATGEN
	
	DWORD RootDirSectors = 0;
	DWORD TmpVal1, TmpVal2, FATSz;

	TmpVal1 = DskSize - ( ReservedSecCnt + RootDirSectors);
	TmpVal2 = (256 * SecPerClus) + NumFATs;
	TmpVal2 = TmpVal2 / 2;
	FATSz = (TmpVal1 + (TmpVal2 - 1)) / TmpVal2;

	return( FatSz );
*/

DWORD get_fat_size_sectors(DWORD DskSize, DWORD ReservedSecCnt, DWORD SecPerClus, DWORD NumFATs, DWORD BytesPerSect)
{
	ULONGLONG Numerator, Denominator;
	ULONGLONG FatElementSize = 4;
	ULONGLONG FatSz;

	// This is based on 
	// http://hjem.get2net.dk/rune_moeller_barnkob/filesystems/fat.html
	// I've made the obvious changes for FAT32
	
	Numerator = FatElementSize * (DskSize - ReservedSecCnt);
	Denominator = (SecPerClus * BytesPerSect) + (FatElementSize * NumFATs);
	
	// round up
	FatSz = (Numerator / Denominator) + 1;

	return (DWORD)FatSz;
}

int write_sect(HWND hWndParent, HANDLE hDevice, DWORD Sector, DWORD BytesPerSector, void *Data, DWORD NumSects)
{
	int64_t ptr = set_file_pointer(hDevice, Sector * BytesPerSector, FILE_BEGIN);
	if (ptr == -1)
	{
		_snwprintf(msg_info, ARRAYSIZE(msg_info), L"FAT32FORMAT: Couldn't seek to offset 0x%09llX in logical drive.", Sector * BytesPerSector);
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		return -1;
	}
	
	DWORD dwWritten = 0;
	int ret = WriteFile(hDevice, Data, NumSects * BytesPerSector, &dwWritten, NULL);
	if (!ret || dwWritten != (NumSects * BytesPerSector))
	{
		_snwprintf(msg_info, ARRAYSIZE(msg_info), L"FAT32FORMAT: Couldn't write %u bytes chunk to sector #%u (%d).", NumSects * BytesPerSector, Sector, GetLastError());
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		return -2;
	}

	return 0;
}

int zero_sectors(HWND hWndParent, HANDLE hDevice, DWORD Sector, DWORD BytesPerSect, DWORD NumSects)
{
	BYTE *pZeroSect = NULL;
	DWORD BurstSize = 4096;
	DWORD WriteSize;
	int ret = 0;
	DWORD dwWritten = 0;
	
	pZeroSect = (BYTE*)VirtualAlloc(NULL, BytesPerSect * BurstSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!pZeroSect)
	{
		MessageBox(hWndParent, TEXT("FAT32FORMAT: Error allocating virtual memory page (pZeroSect)."), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		ret = -1;
		goto out;
	}
	
	int64_t ptr = set_file_pointer(hDevice, Sector * BytesPerSect, FILE_BEGIN);
	if (ptr == -1)
	{
		_snwprintf(msg_info, ARRAYSIZE(msg_info), L"FAT32FORMAT: Couldn't seek to offset 0x%09llX in physical drive.", Sector * BytesPerSect);
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		ret = -2;
		goto out;
	}

	DWORD SectorsToWrite = NumSects;
	while(SectorsToWrite)
	{
		if (SectorsToWrite > BurstSize)
		{
			WriteSize = BurstSize;
		} else {
			WriteSize = SectorsToWrite;
		}
		
		ret = WriteFile(hDevice, pZeroSect, WriteSize * BytesPerSect, &dwWritten, NULL);
		if (!ret || dwWritten != (WriteSize * BytesPerSect))
		{
			_snwprintf(msg_info, ARRAYSIZE(msg_info), L"FAT32FORMAT: Couldn't write %u bytes chunk to sector #%u (%d).", WriteSize * BytesPerSect, Sector + NumSects - SectorsToWrite, GetLastError());
			MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
			ret = -3;
			break;
		} 
		
		SectorsToWrite -= WriteSize;
	}
	
out:
	if (pZeroSect) VirtualFree(pZeroSect, 0, MEM_RELEASE);
	return ret;
}

BYTE get_spc(DWORD ClusterSizeKB, DWORD BytesPerSect)
{
	DWORD spc = ((ClusterSizeKB * 1024) / BytesPerSect);
	return (BYTE)spc;
}

BYTE get_sectors_per_cluster(LONGLONG DiskSizeBytes, DWORD BytesPerSect)
{
	BYTE ret = 0x01; // 1 sector per cluster
	LONGLONG DiskSizeMB = (DiskSizeBytes / (1024 * 1024));

	// 512 MB to 8,191 MB -> 4 KB
	if (DiskSizeMB > 512) ret = get_spc(4, BytesPerSect);  // ret = 0x8;
	
	// 8,192 MB to 16,383 MB -> 8 KB 
	if (DiskSizeMB > 8192) ret = get_spc(8, BytesPerSect); // ret = 0x10;

	// 16,384 MB to 32,767 MB -> 16 KB 
	if (DiskSizeMB > 16384) ret = get_spc(16, BytesPerSect); // ret = 0x20;

	// Larger than 32,768 MB -> 32 KB
	if (DiskSizeMB > 32768) ret = get_spc(32, BytesPerSect);  // ret = 0x40;
	
	return ret;
}

int format_volume(HWND hWndParent, uint32_t drive_num, char *VolLab)
{
	// First open the device
	DWORD i;
	HANDLE hDevice = INVALID_HANDLE_VALUE;
	int cbRet;
	BOOL bRet;
	DISK_GEOMETRY dgDrive;
	BYTE geometry_ex[256]; // DISK_GEOMETRY_EX is variable size
	PDISK_GEOMETRY_EX xdgDrive = (PDISK_GEOMETRY_EX)(void*)geometry_ex;
	PARTITION_INFORMATION piDrive;
	PARTITION_INFORMATION_EX xpiDrive;
	
	// Recommended values
	DWORD ReservedSectCount = 32;
	DWORD NumFATs = 2;
	DWORD BackupBootSect = 6;
	DWORD VolumeId = 0; // calculated before format
	
	// Calculated later
	DWORD FatSize = 0; 
	DWORD BytesPerSect = 0;
	DWORD SectorsPerCluster = 0;
	DWORD TotalSectors = 0;
	DWORD SystemAreaSize = 0;
	DWORD UserAreaSize = 0;

	// structures to be written to the disk
	FAT_BOOTSECTOR32 *pFAT32BootSect = NULL;
	FAT_FSINFO *pFAT32FsInfo = NULL;
	DWORD *pFirstSectOfFat = NULL;
	uint8_t *pFATRootDir = NULL;
	
	int ret = 0;
	VolumeId = get_volume_id();

	// Get the logical path to access the volume
	char *LogicalPath = GetLogicalName(hWndParent, drive_num, FALSE);
	if (LogicalPath == NULL)
	{
		_snwprintf(msg_info, ARRAYSIZE(msg_info), L"No logical drive found for physical drive #%u!", drive_num);
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		ret = -1;
		goto out;
	}
	
	wchar_t devname[128] = {0};
	_snwprintf(devname, ARRAYSIZE(devname), L"%S", LogicalPath);
	free(LogicalPath);
	
#ifdef DEBUG_BUILD
	_snwprintf(msg_info, ARRAYSIZE(msg_info), L"Logical path (physical drive #%u):\n%s", drive_num, devname);
	MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
	
	// Open the volume
	//hDevice = CreateFile(devname, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
	hDevice = CreateFile(devname, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hDevice == INVALID_HANDLE_VALUE)
	{
		_snwprintf(msg_info, ARRAYSIZE(msg_info), L"FAT32FORMAT: Couldn't open logical drive \"%s\" (%d).", devname, GetLastError());
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		ret = -2;
		goto out;
	}
	
	// Disable I/O boundary checks
	bRet = DeviceIoControl(hDevice, FSCTL_ALLOW_EXTENDED_DASD_IO, NULL, 0, NULL, 0, (PDWORD)&cbRet, NULL);
#ifdef DEBUG_BUILD
	if (!bRet)
	{
		_snwprintf(msg_info, ARRAYSIZE(msg_info), L"FAT32FORMAT: Failed to disable I/O boundary checks on logical drive \"%s\" (%d).\nNot a critical error, though.", devname, GetLastError());
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
	}
#endif
	
	// Lock it
	for (i = 0; i < DRIVE_ACCESS_RETRIES; i++)
	{
		bRet = DeviceIoControl(hDevice, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, (PDWORD)&cbRet, NULL);
		if (bRet) break;
		Sleep(DRIVE_ACCESS_TIMEOUT / DRIVE_ACCESS_RETRIES);
	}
	
	if (!bRet)
	{
		_snwprintf(msg_info, ARRAYSIZE(msg_info), L"FAT32FORMAT: Failed to lock logical drive \"%s\" (%d).", devname, GetLastError());
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		ret = -3;
		goto out;
	}
	
	// Try to dismount it from other processes
	bRet = DeviceIoControl(hDevice, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, (PDWORD)&cbRet, NULL);
#ifdef DEBUG_BUILD
	if (!bRet)
	{
		_snwprintf(msg_info, ARRAYSIZE(msg_info), L"FAT32FORMAT: Failed to dismount logical drive \"%s\" (%d).", devname, GetLastError());
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
	}
#endif
	
	// Work out drive params
	bRet = DeviceIoControl(hDevice, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &dgDrive, sizeof(dgDrive), (PDWORD)&cbRet, NULL);
	if (!bRet)
	{
		bRet = DeviceIoControl(hDevice, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, xdgDrive, sizeof(geometry_ex), (PDWORD)&cbRet, NULL);
		if (!bRet)
		{
			_snwprintf(msg_info, ARRAYSIZE(msg_info), L"FAT32FORMAT: Failed to get the device geometry for \"%s\".", devname);
			MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
			ret = -4;
			goto out;
		}
		
		memcpy(&dgDrive, &xdgDrive->Geometry, sizeof(dgDrive));
	}
	
	if (dgDrive.BytesPerSector < 512) dgDrive.BytesPerSector = 512;
	
	bRet = DeviceIoControl(hDevice, IOCTL_DISK_GET_PARTITION_INFO, NULL, 0, &piDrive, sizeof(piDrive), (PDWORD)&cbRet, NULL);
	if (!bRet)
	{
		bRet = DeviceIoControl(hDevice, IOCTL_DISK_GET_PARTITION_INFO_EX, NULL, 0, &xpiDrive, sizeof(xpiDrive), (PDWORD)&cbRet, NULL);
		if (!bRet)
		{
			_snwprintf(msg_info, ARRAYSIZE(msg_info), L"FAT32FORMAT: Failed to get the partition info for \"%s\" (both regular and extended).", devname);
			MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
			ret = -5;
			goto out;
		}
		
		memset(&piDrive, 0, sizeof(piDrive));
		piDrive.StartingOffset.QuadPart = xpiDrive.StartingOffset.QuadPart;
		piDrive.PartitionLength.QuadPart = xpiDrive.PartitionLength.QuadPart;
		piDrive.HiddenSectors = (DWORD)(xpiDrive.StartingOffset.QuadPart / dgDrive.BytesPerSector);
	}

	BytesPerSect = dgDrive.BytesPerSector;

	// Checks on Disk Size
	/*ULONGLONG qTotalSectors = (piDrive.PartitionLength.QuadPart / dgDrive.BytesPerSector);
	if (qTotalSectors >= 0xffffffff)
	{
		// This is a more fundamental limitation on FAT32 - the total sector count in the root dir
		// ís 32bit. With a bit of creativity, FAT32 could be extended to handle at least 2^28 clusters
		// There would need to be an extra field in the FSInfo sector, and the old sector count could
		// be set to 0xffffffff. This is non standard though, the Windows FAT driver FASTFAT.SYS won't
		// understand this. Perhaps a future version of FAT32 and FASTFAT will handle this.
		//die ( "This drive is too big for FAT32 - max 2TB supported\n" );
	}*/
	
	pFAT32BootSect = (FAT_BOOTSECTOR32*)VirtualAlloc(NULL, BytesPerSect, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	pFAT32FsInfo = (FAT_FSINFO*)VirtualAlloc(NULL, BytesPerSect, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	pFirstSectOfFat = (DWORD*)VirtualAlloc(NULL, BytesPerSect, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	pFATRootDir = (uint8_t*)VirtualAlloc(NULL, BytesPerSect, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	
	if (!pFAT32BootSect || !pFAT32FsInfo || !pFirstSectOfFat || !pFATRootDir)
	{
		MessageBox(hWndParent, TEXT("FAT32FORMAT: Failed to allocate memory."), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		ret = -6;
		goto out;
	}
	
	// fill out the boot sector and fs info
	pFAT32BootSect->sJmpBoot[0] = 0xEB;
	pFAT32BootSect->sJmpBoot[1] = 0x58; // jmp.s $+0x5a is 0xeb 0x58, not 0xeb 0x5a. Thanks Marco!
	pFAT32BootSect->sJmpBoot[2] = 0x90;
	memcpy(pFAT32BootSect->sOEMName, "MSWIN4.1", 8);
	pFAT32BootSect->wBytsPerSec = (WORD)BytesPerSect;
	
	SectorsPerCluster = get_sectors_per_cluster(piDrive.PartitionLength.QuadPart, BytesPerSect);

	pFAT32BootSect->bSecPerClus = (BYTE)SectorsPerCluster;
	pFAT32BootSect->wRsvdSecCnt = (WORD)ReservedSectCount;
	pFAT32BootSect->bNumFATs = (BYTE)NumFATs;
	pFAT32BootSect->wRootEntCnt = 0;
	pFAT32BootSect->wTotSec16 = 0;
	pFAT32BootSect->bMedia = 0xF8;
	pFAT32BootSect->wFATSz16 = 0;
	pFAT32BootSect->wSecPerTrk = (WORD)dgDrive.SectorsPerTrack;
	pFAT32BootSect->wNumHeads = (WORD)dgDrive.TracksPerCylinder;
	pFAT32BootSect->dHiddSec = (DWORD)piDrive.HiddenSectors;
	TotalSectors = (DWORD)(piDrive.PartitionLength.QuadPart / dgDrive.BytesPerSector);
	pFAT32BootSect->dTotSec32 = TotalSectors;
	
	FatSize = get_fat_size_sectors(pFAT32BootSect->dTotSec32, pFAT32BootSect->wRsvdSecCnt, pFAT32BootSect->bSecPerClus, pFAT32BootSect->bNumFATs, BytesPerSect);
	
	pFAT32BootSect->dFATSz32 = FatSize;
	pFAT32BootSect->wExtFlags = 0;
	pFAT32BootSect->wFSVer = 0;
	pFAT32BootSect->dRootClus = 2;
	pFAT32BootSect->wFSInfo = 1;
	pFAT32BootSect->wBkBootSec = (WORD)BackupBootSect;
	pFAT32BootSect->bDrvNum = 0x80;
	pFAT32BootSect->Reserved1 = 0;
	pFAT32BootSect->bBootSig = 0x29;
	
	pFAT32BootSect->dBS_VolID = VolumeId;
	memcpy(pFAT32BootSect->sVolLab, VolLab, 11);
	memcpy(pFAT32BootSect->sBS_FilSysType, "FAT32   ", 8);
	((BYTE*)pFAT32BootSect)[510] = 0x55;
	((BYTE*)pFAT32BootSect)[511] = 0xaa;

	/* FATGEN103.DOC says "NOTE: Many FAT documents mistakenly say that this 0xAA55 signature occupies the "last 2 bytes of 
	the boot sector". This statement is correct if - and only if - BPB_BytsPerSec is 512. If BPB_BytsPerSec is greater than 
	512, the offsets of these signature bytes do not change (although it is perfectly OK for the last two bytes at the end 
	of the boot sector to also contain this signature)." 
	
	Windows seems to only check the bytes at offsets 510 and 511. Other OSs might check the ones at the end of the sector,
	so we'll put them there too.
	*/
	
	if (BytesPerSect != 512)
	{
		((BYTE*)pFAT32BootSect)[BytesPerSect-2] = 0x55;
		((BYTE*)pFAT32BootSect)[BytesPerSect-1] = 0xaa;
	}

	// FSInfo sect
	pFAT32FsInfo->dLeadSig = 0x41615252;
	pFAT32FsInfo->dStrucSig = 0x61417272;
	pFAT32FsInfo->dFree_Count = (DWORD) -1;
	pFAT32FsInfo->dNxt_Free = (DWORD) -1;
	pFAT32FsInfo->dTrailSig = 0xaa550000;

	// First FAT Sector
	pFirstSectOfFat[0] = 0x0ffffff8;  // Reserved cluster 1 media id in low byte
	pFirstSectOfFat[1] = 0x0fffffff;  // Reserved cluster 2 EOC
	pFirstSectOfFat[2] = 0x0fffffff;  // end of cluster chain for root dir

	// Write boot sector, fats
	// Sector 0 Boot Sector
	// Sector 1 FSInfo 
	// Sector 2 More boot code - we write zeros here
	// Sector 3 unused
	// Sector 4 unused
	// Sector 5 unused
	// Sector 6 Backup boot sector
	// Sector 7 Backup FSInfo sector
	// Sector 8 Backup 'more boot code'
	// zero'd sectors upto ReservedSectCount
	// FAT1  ReservedSectCount to ReservedSectCount + FatSize
	// ...
	// FATn  ReservedSectCount to ReservedSectCount + FatSize
	// RootDir - allocated to cluster2

	UserAreaSize = (TotalSectors - ReservedSectCount - (NumFATs * FatSize));
	
	// fix up the FSInfo sector
	pFAT32FsInfo->dFree_Count = ((UserAreaSize / SectorsPerCluster) - 1);
	pFAT32FsInfo->dNxt_Free = 3; // clusters 0-1 resered, we used cluster 2 for the root dir

#ifdef DEBUG_BUILD
	// Now we're commited - print some info first
	DWORD ClusterCount = (UserAreaSize / SectorsPerCluster);
	_snwprintf(msg_info, ARRAYSIZE(msg_info), L"Size: %g GB (%u sectors).\n%u bytes per sector, cluster size: %u bytes.\nVolume ID: %04x:%04x.\n%u reserved sectors, %u sectors per FAT, %u FATs.\n%u total clusters, %u free clusters.\nVolume Label: %.11S.", (double)(piDrive.PartitionLength.QuadPart / (1000 * 1000 * 1000)), TotalSectors, BytesPerSect, SectorsPerCluster * BytesPerSect, VolumeId >> 16, VolumeId & 0xffff, ReservedSectCount, FatSize, NumFATs, ClusterCount, pFAT32FsInfo->dFree_Count, VolLab);
	MessageBox(hWndParent, msg_info, TEXT("Debug info"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
	
	// First zero out ReservedSect + FatSize * NumFats + SectorsPerCluster
	SystemAreaSize = (ReservedSectCount + (NumFATs * FatSize) + SectorsPerCluster);
	
	// Once zero_sectors has run, any data on the drive is basically lost....
	if (zero_sectors(hWndParent, hDevice, 0, BytesPerSect, SystemAreaSize) < 0)
	{
		ret = -7;
		goto out;
	}
	
	// Now we should write the boot sector and fsinfo twice, once at 0 and once at the backup boot sect position
	int res = 0;
	for (i = 0; i < 2; i++)
	{
		int SectorStart = (i == 0 ? 0 : BackupBootSect);
		
		res = write_sect(hWndParent, hDevice, SectorStart, BytesPerSect, pFAT32BootSect, 1);
		if (res < 0)
		{
			ret = -8;
			goto out;
		}
		
		res = write_sect(hWndParent, hDevice, SectorStart + 1, BytesPerSect, pFAT32FsInfo, 1);
		if (res < 0)
		{
			ret = -9;
			goto out;
		}
	}
	
	// Write the first fat sector in the right places
	for (i = 0; i < NumFATs; i++)
	{
		int SectorStart = (ReservedSectCount + (i * FatSize));
		res = write_sect(hWndParent, hDevice, SectorStart, BytesPerSect, pFirstSectOfFat, 1);
		if (res < 0)
		{
			ret = -10;
			goto out;
		}
	}
	
	// Set the FAT32 volume label
	memcpy(pFATRootDir, VolLab, 11);
	pFATRootDir[11] = 0x08;
	res = write_sect(hWndParent, hDevice, ReservedSectCount + (NumFATs * FatSize), BytesPerSect, pFATRootDir, 1);
	if (res < 0) ret = -11;
	
out:
	// Unlock device
	if (ret == 0 || ret < -3)
	{
		for (i = 0; i < DRIVE_ACCESS_RETRIES; i++)
		{
			bRet = DeviceIoControl(hDevice, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, (PDWORD)&cbRet, NULL);
			if (bRet) break;
			Sleep(DRIVE_ACCESS_TIMEOUT / DRIVE_ACCESS_RETRIES);
		}
		
		if (!bRet)
		{
			_snwprintf(msg_info, ARRAYSIZE(msg_info), L"FAT32FORMAT: Failed to unlock logical drive \"%s\".", devname);
			MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
			//ret = -12;
		}
	}
	
	// Close device
	if (hDevice != INVALID_HANDLE_VALUE)
	{
		CloseHandle(hDevice);
		hDevice = INVALID_HANDLE_VALUE;
	}
	
	// Free allocated memory
	if (pFAT32BootSect) VirtualFree(pFAT32BootSect, 0, MEM_RELEASE);
	if (pFAT32FsInfo) VirtualFree(pFAT32FsInfo, 0, MEM_RELEASE);
	if (pFirstSectOfFat) VirtualFree(pFirstSectOfFat, 0, MEM_RELEASE);
	if (pFATRootDir) VirtualFree(pFATRootDir, 0, MEM_RELEASE);
	
	return ret;
}
