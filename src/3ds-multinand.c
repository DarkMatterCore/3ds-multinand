#include <windows.h>
#include <commctrl.h>
#include "3ds-multinand.h"

#define MAX_TAG		4
#define TAG_LENGTH	11

extern HWND EmuNANDDriveList, FormatDriveList; // Mapped drives drop-down lists from winmain.c
extern HWND ProgressBar;

uint32_t drive_cnt = 0;
DRIVE_INFO *MultiNANDDrives = NULL;

/* To do: add compatibility with strings from more flashcards */
const uint8_t MAGIC_STR[MAX_TAG][TAG_LENGTH] =	{	{ 0x47, 0x41, 0x54, 0x45, 0x57, 0x41, 0x59, 0x4E, 0x41, 0x4E, 0x44 }, // "GATEWAYNAND"
													{ 0x4D, 0x54, 0x43, 0x41, 0x52, 0x44, 0x5F, 0x4E, 0x41, 0x4E, 0x44 }, // "MTCARD_NAND"
													{ 0x33, 0x44, 0x53, 0x43, 0x41, 0x52, 0x44, 0x4E, 0x41, 0x4E, 0x44 }, // "3DSCARDNAND"
													{ 0x45, 0x4D, 0x55, 0x4E, 0x41, 0x4E, 0x44, 0x39, 0x53, 0x44, 0x20 }  // "EMUNAND9SD "
												};

const uint32_t emunand_sizes[5] = { O3DS_TOSHIBA_NAND, O3DS_SAMSUNG_NAND, N3DS_SAMSUNG_NAND_1, N3DS_SAMSUNG_NAND_2, N3DS_TOSHIBA_NAND };

static wchar_t wc[512] = {0};
static wchar_t msg_info[512] = {0};

wchar_t *SingleDrive;
static wchar_t devname[30] = {0};
static HANDLE drive = INVALID_HANDLE_VALUE;

int format_volume(HWND hWndParent, uint32_t drive_num, char *VolLab);

int GetTextSize(LPTSTR str)
{
	int i = 0;
	while (str[i] != '\0') i++;
	return i;
}

int64_t set_file_pointer(HANDLE h, int64_t new_ptr, uint32_t method)
{
	int32_t hi_ptr = PTR_HIGH(new_ptr);
	int32_t lo_ptr = PTR_LOW(new_ptr);

	lo_ptr = SetFilePointer(h, lo_ptr, (PLONG)&hi_ptr, method);
	if (lo_ptr != -1 && hi_ptr != -1)
	{
/*#ifdef DEBUG_BUILD
		_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"cur_ptr: 0x%08x%08x.", hi_ptr, lo_ptr);
		MessageBox(NULL, msg_info, TEXT("Debug info"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif*/
		return PTR_FULL(hi_ptr, lo_ptr);
	}
	
	return -1;
}

bool write_dummy_data(HANDLE SDcard, int64_t offset)
{
	int64_t ptr = set_file_pointer(SDcard, offset, FILE_BEGIN);
	if (ptr != -1)
	{
		uint32_t bytes = 0;
		uint8_t dummy_buf[SECTOR_SIZE] = {0};
		
		/* Fill buffer with dummy data */
		for (int i = 0; i < SECTOR_SIZE; i += 2)
		{
			dummy_buf[i] = ((DUMMY_DATA >> 8) & 0xff);
			dummy_buf[i+1] = (DUMMY_DATA & 0xff);
		}
		
		/* Write dummy data */
		int res = WriteFile(SDcard, dummy_buf, SECTOR_SIZE, (PDWORD)&bytes, NULL);
		if (res && bytes == SECTOR_SIZE)
		{
/*#ifdef DEBUG_BUILD
			_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Wrote %d bytes long \"0x%04X\" dummy data at offset 0x%09llX.", SECTOR_SIZE, DUMMY_DATA, offset);
			MessageBox(NULL, msg_info, TEXT("Debug info"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif*/
			return true;
		} else {
			MessageBox(NULL, TEXT("Error writing dummy data."), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		}
	} else {
		_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't seek to offset 0x%09llX in physical drive.", offset);
		MessageBox(NULL, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
	}
	
	return false;
}

int64_t CheckStorageCapacity(HWND hWndParent)
{
	uint32_t status, returned;
	DISK_GEOMETRY_EX DiskGeometry;
	
    status = DeviceIoControl(drive, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &DiskGeometry, sizeof(DISK_GEOMETRY_EX), (PDWORD)&returned, NULL);
    if (!status)
	{
#ifdef DEBUG_BUILD
		_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't get the disk geometry (%d).", GetLastError());
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
		return -1;
	}
	
	if (DiskGeometry.DiskSize.QuadPart > 0)
	{
/*#ifdef DEBUG_BUILD
		_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Drive capacity: %I64d bytes.", DiskGeometry.DiskSize.QuadPart);
		MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif*/
		return DiskGeometry.DiskSize.QuadPart;
	}
	
#ifdef DEBUG_BUILD
	MessageBox(hWndParent, TEXT("Drive size is zero!"), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
	return 0;
}

uint32_t GetDriveListIndex(bool format)
{
	uint32_t i;
	
	/* Check which element from the drop-down list was selected */
	/* We'll get the required drive information from the DRIVE_INFO struct */
	SendMessage(format ? FormatDriveList : EmuNANDDriveList, CB_GETLBTEXT, (WPARAM)SendMessage(format ? FormatDriveList : EmuNANDDriveList, CB_GETCURSEL, 0, 0), (LPARAM)wc);
	for (i = 0; i < drive_cnt; i++)
	{
		if (wcsncmp(wc, MultiNANDDrives[i].drive_str, 50) == 0) break;
	}
	
	return i;
}

/* Taken from Rufus source code */
char *GetLogicalName(HWND hWndParent, DWORD DriveIndex, BOOL bKeepTrailingBackslash)
{
	BOOL success = FALSE;
	char volume_name[MAX_PATH];
	HANDLE hDrive = INVALID_HANDLE_VALUE, hVolume = INVALID_HANDLE_VALUE;
	size_t len;
	char path[MAX_PATH];
	VOLUME_DISK_EXTENTS DiskExtents;
	DWORD size;
	UINT drive_type;
	int i, j;
	static const char *ignore_device[] = { "\\Device\\CdRom", "\\Device\\Floppy" };
	static const char *volume_start = "\\\\?\\";
	
	for (i = 0; hDrive == INVALID_HANDLE_VALUE; i++)
	{
		if (i == 0)
		{
			hVolume = FindFirstVolumeA(volume_name, sizeof(volume_name));
			if (hVolume == INVALID_HANDLE_VALUE)
			{
#ifdef DEBUG_BUILD
				_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't access the first GUID volume (%d).", GetLastError());
				MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
				goto out;
			}
		} else {
			if (!FindNextVolumeA(hVolume, volume_name, sizeof(volume_name)))
			{
				if (GetLastError() != ERROR_NO_MORE_FILES)
				{
#ifdef DEBUG_BUILD
					_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't access the next GUID volume (%d).", GetLastError());
					MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
				}
				goto out;
			}
		}
		
		// Sanity checks
		len = strlen(volume_name);
		if ((len <= 1) || (_strnicmp(volume_name, volume_start, 4) != 0) || (volume_name[len-1] != '\\'))
		{
#ifdef DEBUG_BUILD
			_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"'%s' is not a valid GUID volume name.", volume_name);
			MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
			continue;
		}
		
		drive_type = GetDriveTypeA(volume_name);
		if ((drive_type != DRIVE_REMOVABLE) && (drive_type != DRIVE_FIXED)) continue;
		
		volume_name[len-1] = 0;
		
		if (QueryDosDeviceA(&volume_name[4], path, sizeof(path)) == 0)
		{
#ifdef DEBUG_BUILD
			_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Failed to get device path for GUID volume '%s' (%d).", volume_name, GetLastError());
			MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
			continue;
		}
		
		for (j = 0; (j < GET_ARRAYSIZE(ignore_device)) && (_strnicmp(path, ignore_device[j], strlen(ignore_device[j])) != 0); j++);
		
		if (j < GET_ARRAYSIZE(ignore_device))
		{
#ifdef DEBUG_BUILD
			_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Skipping GUID volume for '%s'.", volume_name);
			MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
			continue;
		}
		
		// If we can't have FILE_SHARE_WRITE, forget it
		hDrive = CreateFileA(volume_name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hDrive == INVALID_HANDLE_VALUE)
		{
#ifdef DEBUG_BUILD
			_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't open GUID volume '%s' (%d).", volume_name, GetLastError());
			MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
			continue;
		}
		
		if ((!DeviceIoControl(hDrive, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &DiskExtents, sizeof(DiskExtents), &size, NULL)) || (size <= 0))
		{
#ifdef DEBUG_BUILD
			_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't get disk extents for GUID volume '%s' (%d).", volume_name, GetLastError());
			MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
			CloseHandle(hDrive);
			hDrive = INVALID_HANDLE_VALUE;
			continue;
		}
		
		CloseHandle(hDrive);
		hDrive = INVALID_HANDLE_VALUE;
		
		if ((DiskExtents.NumberOfDiskExtents >= 1) && (DiskExtents.Extents[0].DiskNumber == DriveIndex))
		{
			if (bKeepTrailingBackslash) volume_name[len-1] = '\\';
			success = TRUE;
			break;
		}
	}
	
out:
	if (hVolume != INVALID_HANDLE_VALUE) FindVolumeClose(hVolume);
	return (success ? _strdup(volume_name) : NULL);
}

/* Taken from Rufus source code */
BOOL WaitForLogical(HWND hWndParent, DWORD DriveIndex)
{
	DWORD i;
	char *LogicalPath = NULL;
	
	for (i = 0; i < DRIVE_ACCESS_RETRIES; i++)
	{
		LogicalPath = GetLogicalName(hWndParent, DriveIndex, FALSE);
		if (LogicalPath != NULL)
		{
			free(LogicalPath);
			return TRUE;
		}
		
		Sleep(DRIVE_ACCESS_TIMEOUT / DRIVE_ACCESS_RETRIES);
	}
	
	return FALSE;
}

/* Based on Rufus source code */
BOOL InitializeDisk(HWND hWndParent)
{
	if (drive == INVALID_HANDLE_VALUE) return FALSE;
	
	int dev_res;
	uint32_t res;
	
	CREATE_DISK dsk;
	dsk.PartitionStyle = PARTITION_STYLE_MBR;
	dsk.Mbr.Signature = FAT32_SIGNATURE;
	
	dev_res = DeviceIoControl(drive, IOCTL_DISK_CREATE_DISK, &dsk, sizeof(dsk), NULL, 0, (PDWORD)&res, NULL);
	if (!dev_res)
	{
		_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't delete the drive layout! (%d).", dev_res);
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		return FALSE;
	}
	
	dev_res = DeviceIoControl(drive, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, (PDWORD)&res, NULL);
	if (!dev_res)
	{
		_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't refresh the drive layout! (%d).", dev_res);
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		return FALSE;
	}
	
	return TRUE;
}

/* Based on Rufus source code */
BOOL ClearMBR(HWND hWndParent)
{
	if (drive == INVALID_HANDLE_VALUE) return FALSE;
	
	int dev_res;
	uint32_t res;
	int64_t cur_ptr = -1;
	uint8_t pBuf[SECTOR_SIZE] = {0};
	
	cur_ptr = set_file_pointer(drive, 0, FILE_BEGIN);
	if (cur_ptr == -1)
	{
		_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't set the file pointer to sector #0! (%d)", GetLastError());
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		return FALSE;
	}
	
	dev_res = WriteFile(drive, pBuf, SECTOR_SIZE, (PDWORD)&res, NULL);
	if (!dev_res || res != SECTOR_SIZE)
	{
		_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't write %u bytes block to sector #0! (%d)\nWrote %u bytes.", SECTOR_SIZE, GetLastError(), res);
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		return FALSE;
	}
	
	return TRUE;
}

void RemoveNAND(HWND hWndParent)
{
	int i, dev_res;
	uint32_t res, index = GetDriveListIndex(false);
	int64_t cur_ptr = -1;
	uint8_t buf[SECTOR_SIZE] = {0};
	
	char VolLab[12] = {0};
	snprintf(VolLab, GET_ARRAYSIZE(VolLab), "%.11s", (char*)(&(MAGIC_STR[0][0]))); // Always use the GATEWAYNAND tag
	
	uint32_t DriveLayoutLen = sizeof(DRIVE_LAYOUT_INFORMATION_EX) + (3 * sizeof(PARTITION_INFORMATION_EX));
	DRIVE_LAYOUT_INFORMATION_EX *DriveLayout = malloc(DriveLayoutLen);
	
	if (nandnum > 1)
	{
		/* Check if we're working with an EmuNAND that doesn't exist */
		if (nandnum > MultiNANDDrives[index].emunand_cnt)
		{
#ifdef DEBUG_BUILD
			_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Drive %c: doesn't contain a %d%s EmuNAND!", MultiNANDDrives[index].drive_letter[0], nandnum, NAND_NUM_STR(nandnum));
			MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
			goto out;
		}
	}
	
	/* Generate the required offset */
	int64_t fatsector = (nandnum == 1 ? round4MB(SECTOR_SIZE) : MultiNANDDrives[index].emunand_offsets[nandnum - 1]);
	
	/* Open physical drive */
	_snwprintf(devname, GET_ARRAYSIZE(devname), L"\\\\.\\PhysicalDrive%u", MultiNANDDrives[index].drive_num);
	drive = CreateFile(devname, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (drive == INVALID_HANDLE_VALUE)
	{
		_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't open physical drive \"%s\" (%d).", devname, GetLastError());
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		goto out;
	}
	
	_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Do you want to remove EmuNAND #%d and begin the format procedure?\n\nPlease, bear in mind that doing so will wipe the data on your current partition.\nIf you select \"No\", the process will be canceled.", nandnum);
	dev_res = MessageBox(hWndParent, msg_info, TEXT("Warning"), MB_ICONWARNING | MB_YESNO | MB_SETFOREGROUND);
	if (dev_res == IDYES)
	{
		/* Initialize disk if it doesn't have a MBR */
		if (MultiNANDDrives[index].fat_offset == 0)
		{
			if (!ClearMBR(hWndParent)) goto out;
			if (!InitializeDisk(hWndParent)) goto out;
		}
		
		memset(DriveLayout, 0, DriveLayoutLen);
		
		DriveLayout->PartitionStyle = PARTITION_STYLE_MBR;
		DriveLayout->PartitionCount = 4; // Minimum required by MBR
		DriveLayout->Mbr.Signature = FAT32_SIGNATURE;
		
		DriveLayout->PartitionEntry[0].PartitionStyle = PARTITION_STYLE_MBR;
		DriveLayout->PartitionEntry[0].StartingOffset.QuadPart = fatsector;
		DriveLayout->PartitionEntry[0].PartitionLength.QuadPart = (MultiNANDDrives[index].drive_sz - fatsector);
		DriveLayout->PartitionEntry[0].PartitionNumber = 1;
		
		DriveLayout->PartitionEntry[0].Mbr.PartitionType = PARTITION_FAT32_LBA;
		DriveLayout->PartitionEntry[0].Mbr.BootIndicator = FALSE;
		DriveLayout->PartitionEntry[0].Mbr.RecognizedPartition = 1;
		DriveLayout->PartitionEntry[0].Mbr.HiddenSectors = 0;
		
		for (i = 0; i < 4; i++) DriveLayout->PartitionEntry[i].RewritePartition = TRUE;
		
		dev_res = DeviceIoControl(drive, IOCTL_DISK_SET_DRIVE_LAYOUT_EX, DriveLayout, DriveLayoutLen, NULL, 0, (PDWORD)&res, NULL);
		if (dev_res)
		{
#ifdef DEBUG_BUILD
			_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Partition successfully moved to offset 0x%09llX!", fatsector);
			MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
			
			dev_res = DeviceIoControl(drive, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, (PDWORD)&res, NULL);
			if (!dev_res)
			{
				_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't refresh the partition table! (%d).", dev_res);
				MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
				goto out;
			}
			
			CloseHandle(drive);
			drive = INVALID_HANDLE_VALUE;
			
			/* Wait for the logical drive we just created to appear */
			WaitForLogical(hWndParent, MultiNANDDrives[index].drive_num);
			
			/* Format the new partition */
			dev_res = format_volume(hWndParent, MultiNANDDrives[index].drive_num, VolLab);
			if (dev_res == 0)
			{
				/* Reopen the handle to the physical drive */
				drive = CreateFile(devname, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
				if (drive != INVALID_HANDLE_VALUE)
				{
					/* No point in removing something that doesn't exist */
					if (nandnum == 1 && MultiNANDDrives[index].fat_offset > 0)
					{
						/* Wipe the flashcard tag from the MBR */
						cur_ptr = set_file_pointer(drive, 0, FILE_BEGIN);
						if (cur_ptr != -1)
						{
							dev_res = ReadFile(drive, buf, SECTOR_SIZE, (PDWORD)&res, NULL);
							if (dev_res && res == SECTOR_SIZE)
							{
								/* Replace the first 11 bytes in the buffer with zeroes */
								memset(buf, 0, TAG_LENGTH);
								
								/* Go back to sector #0 */
								cur_ptr = set_file_pointer(drive, 0, FILE_BEGIN);
								if (cur_ptr != -1)
								{
									/* Write the data back to the SD card */
									dev_res = WriteFile(drive, buf, SECTOR_SIZE, (PDWORD)&res, NULL);
									if (!dev_res || res != SECTOR_SIZE)
									{
										MessageBox(hWndParent, TEXT("Error wiping the \"GATEWAYNAND\" tag from the MBR."), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
										goto out;
									}
								} else {
									_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't set the file pointer to zero in \"%s\".", devname);
									MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
									goto out;
								}
							} else {
								_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't read %d bytes chunk from \"%s\" sector #0 (%d).", SECTOR_SIZE, devname, GetLastError());
								MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
								goto out;
							}
						} else {
							_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't set the file pointer to zero in \"%s\".", devname);
							MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
							goto out;
						}
					}
					
					MessageBox(hWndParent, TEXT("Successfully formatted the new FAT partition!"), TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
				} else {
					MessageBox(hWndParent, TEXT("Couldn't reopen the handle to the physical drive!"), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
				}
			} else {
				_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't format the new FAT partition! (%d).", dev_res);
				MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
			}
		} else {
			MessageBox(hWndParent, TEXT("Couldn't modify the drive layout."), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		}
	}
	
out:
	if (DriveLayout) free(DriveLayout);
	
	if (drive != INVALID_HANDLE_VALUE)
	{
		CloseHandle(drive);
		drive = INVALID_HANDLE_VALUE;
	}
}

int WriteReadNANDName(HWND hWndParent, bool read)
{
	int ret = 0;
	uint32_t index = GetDriveListIndex(false);
	bool is_open = (drive != INVALID_HANDLE_VALUE);
	
	/* Check if we're working with an EmuNAND that doesn't exist */
	if (nandnum > MultiNANDDrives[index].emunand_cnt)
	{
		snprintf(nand_name, GET_ARRAYSIZE(nand_name), "EmuNAND #%d not available", nandnum);
		//ret = -2;
		goto out;
	}
	
	if (!is_open)
	{
		/* Open physical drive */
		_snwprintf(devname, GET_ARRAYSIZE(devname), L"\\\\.\\PhysicalDrive%u", MultiNANDDrives[index].drive_num);
		drive = CreateFile(devname, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (drive == INVALID_HANDLE_VALUE)
		{
			_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't open physical drive \"%s\" (%d).", devname, GetLastError());
			MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
			ret = -3;
			goto out;
		}
	}
	
	/* Generate the required offset */
	int64_t mbrsect = MultiNANDDrives[index].emunand_offsets[nandnum - 1];
	
	int64_t ptr = set_file_pointer(drive, mbrsect, FILE_BEGIN);
	if (ptr == -1)
	{
		_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't set the file pointer to sector #%d in drive %c:.", mbrsect / SECTOR_SIZE, MultiNANDDrives[index].drive_letter[0]);
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		ret = -4;
		goto out;
	}
	
	uint32_t res = 0;
	uint8_t buf[SECTOR_SIZE] = {0};
	int dev_res = ReadFile(drive, buf, SECTOR_SIZE, (PDWORD)&res, NULL);
	if (!dev_res || res != SECTOR_SIZE)
	{
		_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't read %d bytes chunk from sector #%d in drive %c: (%d).", SECTOR_SIZE, mbrsect / SECTOR_SIZE, MultiNANDDrives[index].drive_letter[0], GetLastError());
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		ret = -5;
		goto out;
	}
	
	if (memcmp(buf + 16, &(MAGIC_STR[3][0]), TAG_LENGTH) == 0) // "EMUNAND9SD "
	{
		/* Disable name reading/writing. EmuNAND9 isn't compatible with this feature and we may wipe the info it stores in the MBR if we proceed */
		snprintf(nand_name, GET_ARRAYSIZE(nand_name), "Not compatible with EmuNAND9");
	} else {
		if (read)
		{
			if (memcmp(buf + 11, "NAME", 4) == 0)
			{
				strncpy(nand_name, (char*)(buf + 15), NAME_LENGTH - 1);
			} else {
				snprintf(nand_name, GET_ARRAYSIZE(nand_name), "[NO NAME]");
			}
		} else {
			memcpy(buf + 11, "NAME", 4);
			memset(buf + 15, 0x00, NAME_LENGTH);
			strncpy((char*)(buf + 15), nand_name, strlen(nand_name));
			
			/* Go back to the previous sector */
			ptr = set_file_pointer(drive, mbrsect, FILE_BEGIN);
			if (ptr == -1)
			{
				_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't set the file pointer to sector #%d in \"%s\".", mbrsect / SECTOR_SIZE, devname);
				MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
				ret = -4;
				goto out;
			}
			
			dev_res = WriteFile(drive, buf, SECTOR_SIZE, (PDWORD)&res, NULL);
			if (!dev_res || res != SECTOR_SIZE)
			{
				_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't write %d bytes chunk to \"%s\" sector #%d (%d).", SECTOR_SIZE, devname, mbrsect / SECTOR_SIZE, GetLastError());
				MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
				ret = -6;
			} else {
				if (!is_open)
				{
					_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Successfully wrote the %d%s EmuNAND name!", nandnum, NAND_NUM_STR(nandnum));
					MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
				}
			}
		}
	}
	
out:
	if (!is_open && drive != INVALID_HANDLE_VALUE)
	{
		CloseHandle(drive);
		drive = INVALID_HANDLE_VALUE;
	}
	
	return ret;
}

int CheckHeader(uint32_t index, int64_t offset, uint8_t *buf, bool dummy)
{
	/* No point in doing this if we haven't opened a drive */
	if (drive == INVALID_HANDLE_VALUE) return -1;
	
	int dev_res = 0;
	int64_t cur_ptr = -1;
	uint32_t res = 0;
	
	/* Let's make sure we're not setting the file pointer to an invalid location */
	if (offset < MultiNANDDrives[index].fat_offset)
	{
		cur_ptr = set_file_pointer(drive, offset, FILE_BEGIN);
		if (cur_ptr != -1)
		{
			/* Remember that read/write operations must be aligned to 512 bytes */
			dev_res = ReadFile(drive, buf, SECTOR_SIZE, (PDWORD)&res, NULL);
			if (dev_res && res == SECTOR_SIZE)
			{
				if (!dummy)
				{
					/* Check if is this the "NCSD" magic word */
					uint32_t magic_word = 0;
					memcpy(&magic_word, &(buf[0x100]), 4);
					if (magic_word == bswap_32(NCSD_MAGIC))
					{
						return 0;
					} else {
						return -1;
					}
				} else {
					/* Check if this block contains the dummy footer */
					int i;
					uint16_t data = 0;
					for (i = 0; i < SECTOR_SIZE; i += 2)
					{
						memcpy(&data, &(buf[i]), 2);
						if (data != bswap_16(DUMMY_DATA)) break;
					}
					
					if (data == bswap_16(DUMMY_DATA))
					{
						/* Found it */
						return 0;
					} else {
						return -1;
					}
				}
			} else {
				return -3;
			}
		} else {
			return -2;
		}
	}
	
	return -1;
}

uint32_t GetNANDPartitionsSize(uint8_t *buf)
{
	int i;
	uint32_t part_size = 0;
	
	for (i = 0x124; i < 0x160; i += 8)
	{
		uint32_t partition_len = 0;
		memcpy(&partition_len, &(buf[i]), 4);
		part_size += (partition_len * MEDIA_UNIT_SIZE);
	}
	
	return part_size;
}

int GetEmuNANDInfo(HWND hWndParent, uint32_t index, int64_t base_offset, int8_t nand_idx, bool getConsole)
{
	int i, ret = 0;
	int64_t offset;
	uint32_t part_size;
	uint8_t buf[SECTOR_SIZE] = {0};
	bool found = false;
	
	int64_t footer_offset;
	uint8_t dummy_buf[SECTOR_SIZE] = {0};
	
	/* First, check for the presence of an EmuNAND */
	for (i = 0; i < 5; i++)
	{
		/* Skip Old 3DS NAND sizes if we're looking for a New 3DS EmuNAND */
		if (!getConsole && MultiNANDDrives[index].n3ds && i < 2) continue;
		
		/* Only check for an Old 3DS EmuNAND if we're not looking for New 3DS NAND dumps */
		if (!getConsole && !MultiNANDDrives[index].n3ds && i > 1) break;
		
		offset = (base_offset + emunand_sizes[i]);
		if (offset < MultiNANDDrives[index].fat_offset)
		{
			ret = CheckHeader(index, offset, buf, false);
			if (ret == 0)
			{
				found = true;
			} else
			if (ret == -1)
			{
#ifdef DEBUG_BUILD
				_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"%d%ls EmuNAND check: unable to locate the NCSD header at offset 0x%09llX.", nand_idx, NAND_NUM_STR(nand_idx), offset);
				MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
			} else {
				if (ret == -2)
				{
					_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"%d%ls EmuNAND check: couldn't seek to offset 0x%09llX in physical drive.", nand_idx, NAND_NUM_STR(nand_idx), offset);
					MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
				}
				
				if (ret == -3)
				{
					_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"%d%ls EmuNAND check: couldn't read %d bytes block from offset 0x%09llX.", nand_idx, NAND_NUM_STR(nand_idx), SECTOR_SIZE, offset);
					MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
				}
			}
		}
		
		if (found || ret < -1) break;
	}
	
	/* Bail out if there are fatal errors */
	if (ret < -1) return ret;
	
	if (found)
	{
		if (getConsole)
		{
			/* Calculate NAND dump size */
			part_size = GetNANDPartitionsSize(buf);
			
			/* Check if this is a New 3DS NAND dump */
			MultiNANDDrives[index].n3ds = (i > 1);
			
			/* Check if this is a 2DS NAND dump */
			if (MultiNANDDrives[index].n3ds && part_size == O3DS_TOSHIBA_NAND) MultiNANDDrives[index].n2ds = true;
		}
		
		/* Update the DRIVE_INFO struct */
		MultiNANDDrives[index].emunand_cnt++;
		MultiNANDDrives[index].emunand_sizes[nand_idx - 1] = emunand_sizes[i];
		MultiNANDDrives[index].emunand_offsets[nand_idx - 1] = base_offset;
		MultiNANDDrives[index].rednand[nand_idx - 1] = false;
		
#ifdef DEBUG_BUILD
		_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Valid NCSD header detected at offset 0x%09llX.\nConsole: %ls.\nNAND number: %d.\nNAND type: EmuNAND (%ls).\nNAND size: %u bytes.", offset, (MultiNANDDrives[index].n3ds ? (MultiNANDDrives[index].n2ds ? L"2DS" : L"New 3DS") : L"Old 3DS"), nand_idx, NAND_TYPE_STR(MultiNANDDrives[index].emunand_sizes[nand_idx - 1]), MultiNANDDrives[index].emunand_sizes[nand_idx - 1]);
		MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
		
		return 0;
	}
	
	/* Check for the presence of a RedNAND */
	offset = (base_offset + SECTOR_SIZE);
	if (offset < MultiNANDDrives[index].fat_offset)
	{
		ret = CheckHeader(index, offset, buf, false);
		if (ret == 0)
		{
			/* RedNAND size calculation procedure (based in the NCSD header info) */
			/* May not match actual NAND flash capacity */
			part_size = GetNANDPartitionsSize(buf);
			
			if (getConsole)
			{
				/* Check if this is a New 3DS NAND dump */
				MultiNANDDrives[index].n3ds = (part_size >= N3DS_SAMSUNG_NAND_1);
			}
			
			/* Calculate NAND dump size (based in the dummy footer position) */
			for (i = 0; i < 5; i++)
			{
				/* Don't test Old 3DS NAND sizes if we have a New 3DS NCSD header */
				if (MultiNANDDrives[index].n3ds && i < 2) continue;
				
				/* Test New 3DS NAND sizes even if we have an Old 3DS NCSD header */
				/* We could be dealing with a 2DS NAND dump */
				footer_offset = (offset + emunand_sizes[i]);
				if (footer_offset < MultiNANDDrives[index].fat_offset)
				{
					ret = CheckHeader(index, footer_offset, dummy_buf, true);
					if (ret == 0)
					{
						found = true;
					} else
					if (ret == -1)
					{
#ifdef DEBUG_BUILD
						_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"%d%ls RedNAND check: unable to locate the dummy footer at offset 0x%09llX.", nand_idx, NAND_NUM_STR(nand_idx), footer_offset);
						MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
					} else {
						if (ret == -2)
						{
							_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"%d%ls EmuNAND check: couldn't seek to offset 0x%09llX in physical drive.", nand_idx, NAND_NUM_STR(nand_idx), footer_offset);
							MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
						}
						
						if (ret == -3)
						{
							_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"%d%ls EmuNAND check: couldn't read %d bytes block from offset 0x%09llX.", nand_idx, NAND_NUM_STR(nand_idx), SECTOR_SIZE, footer_offset);
							MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
						}
					}
				}
				
				if (found || ret < -1) break;
			}
			
			/* Bail out if there are fatal errors */
			if (ret < -1) return ret;
			
			if (found && getConsole)
			{
				/* Check if this is a 2DS NAND dump */
				MultiNANDDrives[index].n2ds = (!MultiNANDDrives[index].n3ds && i > 1);
				if (MultiNANDDrives[index].n2ds) MultiNANDDrives[index].n3ds = true;
			}
			
#ifdef DEBUG_BUILD
			if (!found) MessageBox(hWndParent, TEXT("Dummy footer not available in the SD card.\nRedNAND size will be calculated based in the NCSD header info.\nIt may not match your actual NAND flash capacity."), TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
			
			/* Update the DRIVE_INFO struct */
			MultiNANDDrives[index].emunand_cnt++;
			MultiNANDDrives[index].emunand_sizes[nand_idx - 1] = (found ? emunand_sizes[i] : part_size);
			MultiNANDDrives[index].emunand_offsets[nand_idx - 1] = base_offset;
			MultiNANDDrives[index].rednand[nand_idx - 1] = true;
			
#ifdef DEBUG_BUILD
			_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Valid NCSD header detected at offset 0x%09llX.\nConsole: %ls.\nNAND number: %d.\nNAND type: RedNAND (%ls).\nNAND size: %u bytes.", offset, (MultiNANDDrives[index].n3ds ? (MultiNANDDrives[index].n2ds ? L"2DS" : L"New 3DS") : L"Old 3DS"), nand_idx, NAND_TYPE_STR(MultiNANDDrives[index].emunand_sizes[nand_idx - 1]), MultiNANDDrives[index].emunand_sizes[nand_idx - 1]);
			MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
			
			return 0;
		} else
		if (ret == -1)
		{
#ifdef DEBUG_BUILD
			_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"%d%ls RedNAND check: unable to locate the NCSD header at offset 0x%09llX.", nand_idx, NAND_NUM_STR(nand_idx), offset);
			MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
		} else {
			if (ret == -2)
			{
				_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"%ld%ls EmuNAND check: couldn't seek to offset 0x%09llX in physical drive.", nand_idx, NAND_NUM_STR(nand_idx), offset);
				MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
			}
			
			if (ret == -3)
			{
				_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"%ld%ls EmuNAND check: couldn't read %d bytes block from offset 0x%09llX.", nand_idx, NAND_NUM_STR(nand_idx), SECTOR_SIZE, offset);
				MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
			}
			
			return ret;
		}
	}
	
	/* No new EmuNAND or RedNAND found */
	return -1;
}

int ParseEmuNANDs(HWND hWndParent, uint32_t index)
{
	/* No point in doing this if we haven't opened a drive */
	if (drive == INVALID_HANDLE_VALUE) return -1;
	
	int i, ret = 0;
	int64_t offset = 0, accum_offset = 0;
	
	/* Try to determine the console type based on the first EmuNAND */
	ret = GetEmuNANDInfo(hWndParent, index, offset, 1, true);
	
	/* Bail out if there are fatal errors or if the first EmuNAND doesn't exist */
	if (ret < 0) goto out;
	
	/* Skip searching for additional EmuNANDs if the FAT partition starting offset matches a 'Legacy' layout NAND size */
	if (MultiNANDDrives[index].fat_offset == O3DS_LEGACY_FAT || MultiNANDDrives[index].fat_offset == N3DS_LEGACY_FAT)
	{
		goto out;
	} else {
		/* Check if the FAT partition starting offset matches a 4MB-rounded NAND size ('Default' and 'Minimum' layouts) */
		bool found = false;
		
		for (i = 0; i < 5; i++)
		{
			if (MultiNANDDrives[index].n3ds && i < 2) continue;
			if (!MultiNANDDrives[index].n3ds && i > 1) break;
			
			if (MultiNANDDrives[index].fat_offset == round4MB(SECTOR_SIZE + emunand_sizes[i]))
			{
				found = true;
				break;
			}
		}
		
		if (found) goto out;
	}
	
	accum_offset = (SECTOR_SIZE + MultiNANDDrives[index].emunand_sizes[0]);
	
	for (i = 1; i < MAX_NAND_NUM; i++)
	{
		/* Check if the current EmuNAND is an O3DS Toshiba EmuNAND, O3DS Samsung EmuNAND, N3DS Samsung EmuNAND (type 1), N3DS Samsung EmuNAND (type 2), N3DS Toshiba EmuNAND */
		/* or a RedNAND (O3DS / N3DS) */
		/* Further iterations perform the same check for additional EmuNANDs, but will only be executed if the previous EmuNAND exists */
		/* It should also be noted that the offsets for the additional EmuNANDs will be calculated based on the 3DS model from which the first EmuNAND was dumped */
		
		/* Depending on the type of the NAND stored in the SD card, the data has to be read in different ways */
		/* This is because the order in which the data is written varies between each type of NAND */
		
		/* EmuNAND: the first 0x200 bytes (NCSD header) are stored **after** the NAND dump. The NAND dump starts to be written */
		/*			from offset 0x200 to the SD card. Used by Gateway and modern CFWs, like Luma3DS and CakesFW */
		
		/* RedNAND: first used by the Palantine CFW, now compatible with more CFWs. It is written to the SD card 'as is', beginning */
		/*			with the NCSD header and following with the rest of the NAND data */
		
		/* Usually, a dummy footer follows afterwards. This applies for both types of NANDs, and serves to indicate the appropiate NAND flash capacity of the 3DS console */
		
		/* Sanity check */
		if (accum_offset >= MultiNANDDrives[index].fat_offset) break;
		
		/* In order to reduce the amount of checks, verify if the previous EmuNAND uses the 'Legacy' layout (rounded to 1GiB or 2GiB boundaries) */
		offset = round_up(accum_offset, (!MultiNANDDrives[index].n3ds ? (int64_t)O3DS_LEGACY_FAT : ((MultiNANDDrives[index].n2ds && MultiNANDDrives[index].emunand_sizes[i - 1] < N3DS_SAMSUNG_NAND_1) ? (int64_t)O3DS_LEGACY_FAT : (int64_t)N3DS_LEGACY_FAT)));
		ret = GetEmuNANDInfo(hWndParent, index, offset, i + 1, false);
		if (ret < -1)
		{
			/* Bail out if there are fatal errors */
			break;
		} else
		if (ret == -1)
		{
			/* Check if the previous EmuNAND uses either the 'Default' or 'Minimum' layout */
			offset = round4MB(accum_offset);
			ret = GetEmuNANDInfo(hWndParent, index, offset, i + 1, false);
			
			/* Bail out if there are fatal errors or if we didn't detect the current EmuNAND */
			if (ret < 0) break;
		}
		
		/* Update the offset and look for the next EmuNAND if we found one */
		accum_offset = (offset + SECTOR_SIZE + MultiNANDDrives[index].emunand_sizes[i]);
	}
	
out:
	if (ret < -1) return -1;
	
	return 0;
}

int ParseDrives(HWND hWndParent, bool check_fixed)
{
	int i, j = 0, dev_res;
	int64_t cur_ptr = -1;
	uint8_t buf[SECTOR_SIZE] = {0};
	
	/* Get the total amount of available logical drives */
	wchar_t LogicalDrives[MAX_PATH] = {0};
	uint32_t res = GetLogicalDriveStrings(MAX_PATH, LogicalDrives);
	if (res > 0 && res <= MAX_PATH)
	{
		/* Free the previous drive list buffer (if it has been allocated) */
		if (MultiNANDDrives)
		{
			free(MultiNANDDrives);
			MultiNANDDrives = NULL;
		}
		
		/* GetLogicalDriveStrings() returns the total amount of characters copied to the buffer, not the number of drives */
		/* Not a problem, though */
		drive_cnt = (res / 4);
		
		/* Allocate memory for the drive list */
		MultiNANDDrives = malloc(drive_cnt * sizeof(DRIVE_INFO));
		if (MultiNANDDrives)
		{
			memset(MultiNANDDrives, 0, drive_cnt * sizeof(DRIVE_INFO));
			
			/* Try to open each logical drive through CreateFile(), in order to get their physical drive number */
			/* This is because we won't operate on a filesystem level */
			/* Once we get the physical drive number, we'll close the current handle and try to open the disk as a physical media */
			SingleDrive = LogicalDrives;
			while (*SingleDrive)
			{
				/* Skip drives A:, B: and C: to avoid problems */
				if (SingleDrive[0] != L'A' && SingleDrive[0] != L'B' && SingleDrive[0] != L'C')
				{
					/* Get the drive type */
					res = GetDriveType(SingleDrive);
					if ((check_fixed && (res == DRIVE_REMOVABLE || res == DRIVE_FIXED)) || (!check_fixed && res == DRIVE_REMOVABLE))
					{
						/* Open logical drive */
						_snwprintf(devname, GET_ARRAYSIZE(devname), L"\\\\.\\%c:", SingleDrive[0]);
						drive = CreateFile(devname, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
						if (drive != INVALID_HANDLE_VALUE)
						{
							/* Check if the drive is ready */
							dev_res = DeviceIoControl(drive, IOCTL_STORAGE_CHECK_VERIFY, NULL, 0, NULL, 0, (PDWORD)&res, NULL);
							if (dev_res != 0)
							{
								VOLUME_DISK_EXTENTS diskExtents;
								dev_res = DeviceIoControl(drive, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, (void*)&diskExtents, (uint32_t)sizeof(diskExtents), (PDWORD)&res, NULL);
								if (dev_res && res > 0)
								{
									/* Then again, we don't want to run the code on drive #0. This may be an additional partition placed after C: */
									/* Also, skip the drive if the number of extents is >= 2, since it could represent a RAID setup */
									if (diskExtents.NumberOfDiskExtents == 1 && diskExtents.Extents[0].DiskNumber > 0)
									{
										/* Get the starting offset of the partition */
										int64_t p_offset;
										PARTITION_INFORMATION_EX xpiDrive;
										
										dev_res = DeviceIoControl(drive, IOCTL_DISK_GET_PARTITION_INFO_EX, NULL, 0, (void*)&xpiDrive, (uint32_t)sizeof(xpiDrive), (PDWORD)&res, NULL);
										if (dev_res && res > 0)
										{
											/* Save the partition starting offset */
											p_offset = xpiDrive.StartingOffset.QuadPart;
											
											/* Close logical drive */
											CloseHandle(drive);
											drive = INVALID_HANDLE_VALUE;
											
											/* Open physical drive */
											_snwprintf(devname, GET_ARRAYSIZE(devname), L"\\\\.\\PhysicalDrive%u", (unsigned int)diskExtents.Extents[0].DiskNumber);
											drive = CreateFile(devname, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
											if (drive != INVALID_HANDLE_VALUE)
											{
												/* Check drive capacity */
												int64_t drive_sz = CheckStorageCapacity(hWndParent);
												if (drive_sz > GIBIBYTE)
												{
													/* Check if this SD card contains an EmuNAND */
													/* Just for safety, let's set the file pointer to zero before attempting to read */
													cur_ptr = set_file_pointer(drive, 0, FILE_BEGIN);
													if (cur_ptr != -1)
													{
														/* Read operations have to be aligned to 512 bytes in order to get this to work */
														dev_res = ReadFile(drive, buf, SECTOR_SIZE, (PDWORD)&res, NULL);
														if (dev_res && res == SECTOR_SIZE)
														{
															bool contains_tag = false;
															
															for (i = 0; i < MAX_TAG; i++)
															{
																if (memcmp(buf, &(MAGIC_STR[i][0]), TAG_LENGTH) == 0)
																{
																	contains_tag = true;
																	break;
																}
															}
#ifdef DEBUG_BUILD
															if (!contains_tag && p_offset > 0)
															{
																_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"MBR signature (logical drive \"%c:\"):\n", SingleDrive[0]);
																for (i = 0; i < TAG_LENGTH; i++)
																{
																	wchar_t byte[4] = {0};
																	_snwprintf(byte, 3, L"%02X ", buf[i]);
																	wcscat(msg_info, byte);
																}
																MessageBox(hWndParent, msg_info, TEXT("Debug info"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
															}
#endif
															/* Check if we already added this physical drive to the list */
															bool found = false;
															
															if (j > 0)
															{
																for (i = 0; i < j; i++)
																{
																	if (MultiNANDDrives[i].drive_num == (unsigned int)diskExtents.Extents[0].DiskNumber)
																	{
																		found = true;
																		break;
																	}
																}
															}
															
															if (j == 0 || (j > 0 && !found))
															{
																/* Add this drive to the drop-down list */
																_snwprintf(MultiNANDDrives[j].drive_str, 50, L"%c: (Disk #%u) (%.2f GiB)\0", SingleDrive[0], (unsigned int)diskExtents.Extents[0].DiskNumber, ((double)drive_sz) / GIBIBYTE);
																SendMessage(contains_tag ? EmuNANDDriveList : FormatDriveList, CB_ADDSTRING, 0, (LPARAM)MultiNANDDrives[j].drive_str);
																
																/* Fill the rest of the info */
																_snwprintf(MultiNANDDrives[j].drive_letter, GET_ARRAYSIZE(MultiNANDDrives[j].drive_letter), L"%c:\\", SingleDrive[0]);
																MultiNANDDrives[j].drive_num = (unsigned int)diskExtents.Extents[0].DiskNumber;
																MultiNANDDrives[j].drive_sz = drive_sz;
																MultiNANDDrives[j].fat_offset = p_offset;
																
																/* Parse any EmuNANDs this drive may contain */
																if (contains_tag) ParseEmuNANDs(hWndParent, j);
																
																/* Increment the list index */
																j++;
															} else {
																/* Determine the position of the string we need to update in the drop-down list */
																wchar_t tmp[50] = {0};
																int k, list_cnt = SendMessage(contains_tag ? EmuNANDDriveList : FormatDriveList, CB_GETCOUNT, 0, 0);
																for (k = 0; k < list_cnt; k++)
																{
																	SendMessage(contains_tag ? EmuNANDDriveList : FormatDriveList, CB_GETLBTEXT, (WPARAM)k, (LPARAM)tmp);
																	if (wcsncmp(tmp, MultiNANDDrives[i].drive_str, 50) == 0) break;
																}
																
																/* Get the position of the first '(' character in the drive string we need to update */
																wchar_t *pwc = wcschr(MultiNANDDrives[i].drive_str, L'(');
																
																/* Add the partition letter we just found to the string */
																_snwprintf(tmp, 50, L"%.*ls, %c: %ls", (int)(pwc - MultiNANDDrives[i].drive_str - 1), MultiNANDDrives[i].drive_str, SingleDrive[0], MultiNANDDrives[i].drive_str + (int)(pwc - MultiNANDDrives[i].drive_str));
																_snwprintf(MultiNANDDrives[i].drive_str, 50, L"%ls\0", tmp);
																
																/* Update the drive info in the drop-down list */
																SendMessage(contains_tag ? EmuNANDDriveList : FormatDriveList, CB_DELETESTRING, (WPARAM)k, 0);
																SendMessage(contains_tag ? EmuNANDDriveList : FormatDriveList, CB_INSERTSTRING, (WPARAM)k, (LPARAM)MultiNANDDrives[i].drive_str);
															}
														} else {
#ifdef DEBUG_BUILD
															_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't read %d bytes chunk from \"%s\" sector #0 (%d).", SECTOR_SIZE, devname, GetLastError());
															MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
														}
													} else {
#ifdef DEBUG_BUILD
														_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't set the file pointer to zero in \"%s\".", devname);
														MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
													}
												} else {
#ifdef DEBUG_BUILD
													if (drive_sz <= 0)
													{
														_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't get the drive capacity for \"%s\".", devname);
														MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
													}
#endif
												}
											} else {
#ifdef DEBUG_BUILD
												_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't open physical drive \"%s\" (%d).", devname, GetLastError());
												MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
											}
										} else {
#ifdef DEBUG_BUILD
											_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't retrieve extended partition information for \"%s\" (%d).", devname, GetLastError());
											MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
										}
									}
								} else {
#ifdef DEBUG_BUILD
									_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't retrieve disk volume information for \"%s\".\ndev_res: %d / res: %u.", devname, dev_res, res);
									MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
								}
							} else {
#ifdef DEBUG_BUILD
								_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Logical drive \"%c:\" not ready (empty drive?).", SingleDrive[0]);
								MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
							}
						} else {
#ifdef DEBUG_BUILD
							_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't open logical drive \"%s\" (%d).", devname, GetLastError());
							MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
						}
					} else {
#ifdef DEBUG_BUILD
						if (res == DRIVE_UNKNOWN)
						{
							_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Unknown drive type for \"%c:\".", SingleDrive[0]);
							MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
						}
#endif
					}
				}
				
				/* Close current drive */
				if (drive != INVALID_HANDLE_VALUE)
				{
					CloseHandle(drive);
					drive = INVALID_HANDLE_VALUE;
				}
				
				/* Get the next drive */
				SingleDrive += GetTextSize(SingleDrive) + 1;
			}
		} else {
			MessageBox(hWndParent, TEXT("Couldn't allocate memory for the drive list."), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
			return -1;
		}
	} else {
		MessageBox(hWndParent, TEXT("Couldn't parse logical drive strings."), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		return -1;
	}
	
	/* Resize the allocated memory block (in case we didn't use all the elements in the array) */
	if (j > 0)
	{
		if (j < drive_cnt)
		{
			MultiNANDDrives = realloc(MultiNANDDrives, j * sizeof(DRIVE_INFO));
			drive_cnt = (unsigned int)j;
		}
		
#ifdef DEBUG_BUILD
		for (i = 0; i < j; i++)
		{
			_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Drive #%d info:\n\n- String: \"%ls\".\n- Letter: %ls.\n- Number: %u.\n- Capacity: %I64d bytes.\n- Partition offset: 0x%09llX.", i + 1, MultiNANDDrives[i].drive_str, MultiNANDDrives[i].drive_letter, MultiNANDDrives[i].drive_num, MultiNANDDrives[i].drive_sz, MultiNANDDrives[i].fat_offset);
			MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
		}
#endif
	} else {
		if (MultiNANDDrives)
		{
			free(MultiNANDDrives);
			MultiNANDDrives = NULL;
		}
	}
	
	return 0;
}

void InjectExtractNAND(wchar_t *fname, HWND hWndParent, bool isFormat)
{
	bool n3ds, n2ds = false;
	int i, dev_res;
	FILE *nandfile = NULL;
	uint8_t buf[SECTOR_SIZE] = {0};
	int64_t cur_ptr = -1, fatsector = 0, nandsector = 0, offset = 0;
	uint32_t res, index = GetDriveListIndex(isFormat), nandsize = 0, magic_word = 0, real_nandsize = 0;
	
	uint32_t DriveLayoutLen = sizeof(DRIVE_LAYOUT_INFORMATION_EX) + (3 * sizeof(PARTITION_INFORMATION_EX));
	DRIVE_LAYOUT_INFORMATION_EX *DriveLayout = malloc(DriveLayoutLen);
	
	/* Open 3DS NAND dump */
	nandfile = _wfopen(fname, (is_input ? L"rb" : L"wb"));
	if (!nandfile)
	{
		_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't open \"%s\" for %s.", fname, (is_input ? L"reading" : L"writing"));
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		goto out;
	}
	
	if (is_input)
	{
		/* Check if we're trying to inject an EmuNAND to a slot after the right next one */
		if (!isFormat && nandnum > (MultiNANDDrives[index].emunand_cnt + 1))
		{
			_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Inject a %d%ls EmuNAND to drive %c: first!", MultiNANDDrives[index].emunand_cnt + 1, NAND_NUM_STR(MultiNANDDrives[index].emunand_cnt + 1), MultiNANDDrives[index].drive_letter[0]);
			MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
			goto out;
		}
		
		/* Store NAND dump size */
		fseek(nandfile, 0, SEEK_END);
		nandsize = ftell(nandfile);
		rewind(nandfile);
		
		n3ds = (nandsize == N3DS_SAMSUNG_NAND_1 || nandsize == (N3DS_SAMSUNG_NAND_1 + SECTOR_SIZE) || nandsize == N3DS_SAMSUNG_NAND_2 || nandsize == (N3DS_SAMSUNG_NAND_2 + SECTOR_SIZE) || nandsize == N3DS_TOSHIBA_NAND || nandsize == (N3DS_TOSHIBA_NAND + SECTOR_SIZE));
		if ((nandsize != O3DS_TOSHIBA_NAND && nandsize != (O3DS_TOSHIBA_NAND + SECTOR_SIZE) && nandsize != O3DS_SAMSUNG_NAND && nandsize != (O3DS_SAMSUNG_NAND + SECTOR_SIZE) && nandsize != N3DS_SAMSUNG_NAND_1 && nandsize != (N3DS_SAMSUNG_NAND_1 + SECTOR_SIZE) && nandsize != N3DS_SAMSUNG_NAND_2 && nandsize != (N3DS_SAMSUNG_NAND_2 + SECTOR_SIZE) && nandsize != N3DS_TOSHIBA_NAND && nandsize != (N3DS_TOSHIBA_NAND + SECTOR_SIZE)) || (!isFormat && !MultiNANDDrives[index].n2ds && n3ds != MultiNANDDrives[index].n3ds))
		{
			_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Invalid 3DS NAND dump.\nFilesize (%u bytes) is invalid.", nandsize);
			MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
			goto out;
		}
		
		bool is_rednand = (nandsize == (O3DS_TOSHIBA_NAND + SECTOR_SIZE) || nandsize == (O3DS_SAMSUNG_NAND + SECTOR_SIZE) || nandsize == (N3DS_SAMSUNG_NAND_1 + SECTOR_SIZE) || nandsize == (N3DS_SAMSUNG_NAND_2 + SECTOR_SIZE) || nandsize == (N3DS_TOSHIBA_NAND + SECTOR_SIZE));
		
		/* Check if the supplied NAND dump does contain an NCSD header */
		fseek(nandfile, (is_rednand ? (SECTOR_SIZE + 0x100) : 0x100), SEEK_SET);
		fread(&magic_word, 4, 1, nandfile);
		rewind(nandfile);
		
		if (magic_word == bswap_32(NCSD_MAGIC))
		{
			if (is_rednand)
			{
				/* Skip the dummy header (if it's already present) */
				fseek(nandfile, SECTOR_SIZE, SEEK_SET);
				nandsize -= SECTOR_SIZE;
			}
		} else {
			MessageBox(hWndParent, TEXT("Invalid 3DS NAND dump.\nThe NCSD header is missing."), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
			goto out;
		}
		
		if (is_rednand && !cfw)
		{
			/* Warn the user about the use of an input RedNAND */
			dev_res = MessageBox(hWndParent, TEXT("The selected input NAND dump was previously patched with the \"drag_emunand_here\" batch script, and therefore, is a RedNAND.\n\nDo you want to inject this file as a RedNAND?\nIf you select \"No\", the NAND dump will be written as a common EmuNAND."), TEXT("Warning"), MB_ICONWARNING | MB_YESNO | MB_SETFOREGROUND);
			
			/*  Override configuration */
			cfw = (dev_res == IDYES);
		}
		
#ifdef DEBUG_BUILD
		_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"%s 3DS %s NAND dump detected!\nFilesize: %u bytes.\nValid NCSD header detected at offset 0x%08x.", (!n3ds ? L"Old" : L"New"), NAND_TYPE_STR(nandsize), nandsize, (is_rednand ? (SECTOR_SIZE + 0x100) : 0x100));
		MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
		
		if (isFormat)
		{
			/* Adjust NAND size to the minimum possible */
			
			if (!n3ds)
			{
				nandsize = O3DS_TOSHIBA_NAND;
			} else {
				fread(buf, SECTOR_SIZE, 1, nandfile);
				fseek(nandfile, (is_rednand ? SECTOR_SIZE : 0), SEEK_SET);
				
				/* Determine the NAND CTR FAT size (2DS NAND dump check) */
				uint32_t part_size = GetNANDPartitionsSize(buf);
				n2ds = (part_size == O3DS_TOSHIBA_NAND);
				
				nandsize = (n2ds ? O3DS_TOSHIBA_NAND : N3DS_SAMSUNG_NAND_1);
			}
		} else {
			/* Adjust 2DS status */
			if (n3ds) n2ds = MultiNANDDrives[index].n2ds;
			
			if (nandnum > MultiNANDDrives[index].emunand_cnt)
			{
				/* Verify if the size of the NAND #1 is greater than the size of the new NAND */
				/* This will let us add zero padding after writing the whole file */
				/* We do it this way to avoid problems with CFWs that do not support varying NAND sizes */
				if (MultiNANDDrives[index].emunand_sizes[0] > nandsize) real_nandsize = MultiNANDDrives[index].emunand_sizes[0];
			} else {
				/* Verify if the size of the already injected NAND is greater than the size of the new NAND */
				/* This will let us add zero padding after writing the whole file */
				if (MultiNANDDrives[index].emunand_sizes[nandnum - 1] > nandsize) real_nandsize = MultiNANDDrives[index].emunand_sizes[nandnum - 1];
			}
		}
		
		/* Adjust RedNAND configuration */
		if (isFormat) cfw = true;
	} else {
		/* Check if we're trying to extract an EmuNAND that doesn't exist */
		if (nandnum > MultiNANDDrives[index].emunand_cnt)
		{
#ifdef DEBUG_BUILD
			_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Drive %c: doesn't contain a %d%s EmuNAND!", MultiNANDDrives[index].drive_letter[0], nandnum, NAND_NUM_STR(nandnum));
			MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
			goto out;
		}
		
		/* Adjust New 3DS status */
		n3ds = MultiNANDDrives[index].n3ds;
		
		/* Adjust 2DS status */
		n2ds = MultiNANDDrives[index].n2ds;
		
		/* Adjust NAND size */
		nandsize = MultiNANDDrives[index].emunand_sizes[nandnum - 1];
		
		/* Adjust RedNAND configuration */
		cfw = MultiNANDDrives[index].rednand[nandnum - 1];
	}
	
	/* Generate required offsets */
	if (!isFormat)
	{
		if (nandnum > MultiNANDDrives[index].emunand_cnt)
		{
			/* Use the 'Default' layout */
			/* Always force the creation of a new, 4 MiB aligned offset */
			fatsector = round4MB(MultiNANDDrives[index].fat_offset + SECTOR_SIZE + (real_nandsize > 0 ? real_nandsize : nandsize));
			nandsector = (MultiNANDDrives[index].fat_offset + SECTOR_SIZE);
		} else {
			/* Use the current layout */
			fatsector = MultiNANDDrives[index].fat_offset;
			nandsector = (MultiNANDDrives[index].emunand_offsets[nandnum - 1] + SECTOR_SIZE);
		}
	} else {
		/* Use the 'Minimum' layout */
		fatsector = ((!n3ds || n2ds) ? (int64_t)O3DS_MINIMUM_FAT : (int64_t)N3DS_MINIMUM_FAT);
		nandsector = (int64_t)SECTOR_SIZE;
	}
	
	/* Check storage capacity */
	if (MultiNANDDrives[index].drive_sz <= fatsector)
	{
		_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Your SD card must have a capacity of at least %u GiB to store the %d%s EmuNAND.", CAPACITY(nandnum, (n3ds && !n2ds)), nandnum, NAND_NUM_STR(nandnum));
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		goto out;
	}
	
#ifdef DEBUG_BUILD
	_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Expected FAT partition starting offset: 0x%09llX.\nExpected NAND starting offset: 0x%09llX.", fatsector, nandsector);
	MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
	
	/* Open physical drive */
	_snwprintf(devname, GET_ARRAYSIZE(devname), L"\\\\.\\PhysicalDrive%u", MultiNANDDrives[index].drive_num);
	drive = CreateFile(devname, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (drive == INVALID_HANDLE_VALUE)
	{
		_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't open physical drive \"%s\" (%d).", devname, GetLastError());
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		goto out;
	}
	
	/* Lock access to the physical drive */
	/*for (i = 0; i < DRIVE_ACCESS_RETRIES; i++)
	{
		dev_res = DeviceIoControl(drive, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, (PDWORD)&res, NULL);
		if (dev_res) break;
		Sleep(DRIVE_ACCESS_TIMEOUT / DRIVE_ACCESS_RETRIES);
	}
	
	if (!dev_res)
	{
		_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Failed to lock \"%s\".", devname);
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		goto out;
	}*/
	
	/* Try to dismount it from other processes */
	/*dev_res = DeviceIoControl(drive, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, (PDWORD)&res, NULL);
#ifdef DEBUG_BUILD
	if (!dev_res)
	{
		_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Failed to dismount \"%s\" (%d).", devname, GetLastError());
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
	}
#endif*/
	
	/* Check if the SD card is write-protected */
	if (is_input)
	{
		uint32_t sys_flags = 0;
		dev_res = GetVolumeInformation(MultiNANDDrives[index].drive_letter, NULL, 0, NULL, NULL, (PDWORD)&sys_flags, NULL, 0);
		if (dev_res != 0)
		{
			if (sys_flags & FILE_READ_ONLY_VOLUME)
			{
				MessageBox(hWndParent, TEXT("The SD card is write-protected!\nMake sure the lock slider is not in the \"locked\" position."), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
				goto out;
			}
		} else {
#ifdef DEBUG_BUILD
			MessageBox(hWndParent, TEXT("Couldn't get the file system flags.\nNot really a critical problem. Process won't be halted."), TEXT("Error"), MB_ICONWARNING | MB_OK | MB_SETFOREGROUND);
#endif
		}
	}
	
	/* Get drive layout */
	dev_res = DeviceIoControl(drive, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, DriveLayout, DriveLayoutLen, (PDWORD)&res, NULL);
	if (dev_res)
	{
		if (DriveLayout->PartitionStyle == PARTITION_STYLE_MBR || (isFormat && DriveLayout->PartitionStyle != PARTITION_STYLE_MBR))
		{
			char VolLab[12] = {0};
			snprintf(VolLab, GET_ARRAYSIZE(VolLab), "%.11s", (char*)(&(MAGIC_STR[0][0]))); // Always use the GATEWAYNAND tag
			
			if (!isFormat && DriveLayout->PartitionStyle == PARTITION_STYLE_MBR && DriveLayout->PartitionEntry[0].Mbr.PartitionType != PARTITION_FAT32 && DriveLayout->PartitionEntry[0].Mbr.PartitionType != PARTITION_FAT32_LBA && DriveLayout->PartitionEntry[0].Mbr.PartitionType != PARTITION_FAT_16 && DriveLayout->PartitionEntry[0].Mbr.PartitionType != PARTITION_FAT16B)
			{
				_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Detected partition type: 0x%02X.\nYour SD card isn't currently using a FAT partition.\nThis is weird, considering that the SD card already contains an EmuNAND.\nThis means it will *not* work properly with your 3DS console.\n\n", DriveLayout->PartitionEntry[0].Mbr.PartitionType);
				
				if (is_input)
				{
					wcscat(msg_info, L"Do you want to format this partition to FAT?\nPlease, bear in mind that doing so will wipe the data on your current partition.\nIf you select \"No\", the process will be canceled.");
					dev_res = MessageBox(hWndParent, msg_info, TEXT("Warning"), MB_ICONWARNING | MB_YESNO | MB_SETFOREGROUND);
					if (dev_res == IDNO)
					{
						goto out;
					} else {
						CloseHandle(drive);
						drive = INVALID_HANDLE_VALUE;
						memset(DriveLayout, 0, DriveLayoutLen);
						
						/* Format the new partition */
						dev_res = format_volume(hWndParent, MultiNANDDrives[index].drive_num, VolLab);
						if (dev_res == 0)
						{
							/* Reopen the handle to the physical drive */
							drive = CreateFile(devname, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
							if (drive != INVALID_HANDLE_VALUE)
							{
								/* Get drive layout */
								dev_res = DeviceIoControl(drive, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, DriveLayout, DriveLayoutLen, (PDWORD)&res, NULL);
								if (dev_res)
								{
									MessageBox(hWndParent, TEXT("Successfully formatted the new FAT partition!"), TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
								} else {
									_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't get the drive layout (%d).", GetLastError());
									MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
									goto out;
								}
							} else {
								MessageBox(hWndParent, TEXT("Couldn't reopen the handle to the physical drive!"), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
								goto out;
							}
						}
					}
				} else {
					wcscat(msg_info, L"Nonetheless, the extraction process will continue. But be warned.");
					MessageBox(hWndParent, msg_info, TEXT("Warning"), MB_ICONWARNING | MB_OK | MB_SETFOREGROUND);
				}
			}
			
			if (nandnum > 1 || isFormat)
			{
				/* Force the format procedure to run even if the starting offset is right. The partition table may have been successfully modified in a previous run, yet the format itself might have failed */
				/* It is also possible but less likely that the starting offset is right and the partition type is not FAT */
				if (DriveLayout->PartitionEntry[0].StartingOffset.QuadPart < fatsector || isFormat)
				{
					if (is_input)
					{
						if (DriveLayout->PartitionEntry[0].StartingOffset.QuadPart < fatsector)
						{
							/* Move filesystem to the right */
							_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Partition detected below the required offset (at 0x%09llX).\nDo you want to begin the drive layout modification procedure?\n\nPlease, bear in mind that doing so will wipe the data on your current partition.\nIf you select \"No\", the process will be canceled.", DriveLayout->PartitionEntry[0].StartingOffset.QuadPart);
						} else {
							_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Do you want to begin the format procedure?\n\nPlease, bear in mind that doing so will wipe the data on your current partition.\nIf you select \"No\", the process will be canceled.");
						}
						
						dev_res = MessageBox(hWndParent, msg_info, TEXT("Warning"), MB_ICONWARNING | MB_YESNO | MB_SETFOREGROUND);
						if (dev_res == IDNO)
						{
							goto out;
						} else {
							/* Initialize disk if it doesn't have a MBR */
							if (DriveLayout->PartitionEntry[0].StartingOffset.QuadPart == 0)
							{
								if (!ClearMBR(hWndParent)) goto out;
								if (!InitializeDisk(hWndParent)) goto out;
							}
							
							memset(DriveLayout, 0, DriveLayoutLen);
							
							DriveLayout->PartitionStyle = PARTITION_STYLE_MBR;
							DriveLayout->PartitionCount = 4; // Minimum required by MBR
							DriveLayout->Mbr.Signature = FAT32_SIGNATURE;
							
							DriveLayout->PartitionEntry[0].PartitionStyle = PARTITION_STYLE_MBR;
							DriveLayout->PartitionEntry[0].StartingOffset.QuadPart = fatsector;
							DriveLayout->PartitionEntry[0].PartitionLength.QuadPart = (MultiNANDDrives[index].drive_sz - fatsector);
							DriveLayout->PartitionEntry[0].PartitionNumber = 1;
							
							DriveLayout->PartitionEntry[0].Mbr.PartitionType = PARTITION_FAT32_LBA;
							DriveLayout->PartitionEntry[0].Mbr.BootIndicator = FALSE;
							DriveLayout->PartitionEntry[0].Mbr.RecognizedPartition = 1;
							DriveLayout->PartitionEntry[0].Mbr.HiddenSectors = 0;
							
							for (i = 0; i < 4; i++) DriveLayout->PartitionEntry[i].RewritePartition = TRUE;
							
							dev_res = DeviceIoControl(drive, IOCTL_DISK_SET_DRIVE_LAYOUT_EX, DriveLayout, DriveLayoutLen, NULL, 0, (PDWORD)&res, NULL);
							if (dev_res)
							{
#ifdef DEBUG_BUILD
								_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Partition successfully moved to offset 0x%09llX!", fatsector);
								MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
								
								dev_res = DeviceIoControl(drive, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, (PDWORD)&res, NULL);
								if (!dev_res)
								{
									_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't refresh the partition table! (%d).", dev_res);
									MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
									goto out;
								}
								
								CloseHandle(drive);
								drive = INVALID_HANDLE_VALUE;
								
								/* Wait for the logical drive we just created to appear */
								WaitForLogical(hWndParent, MultiNANDDrives[index].drive_num);
								
								/* Format the new partition */
								dev_res = format_volume(hWndParent, MultiNANDDrives[index].drive_num, VolLab);
								if (dev_res == 0)
								{
									/* Reopen the handle to the physical drive */
									drive = CreateFile(devname, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
									if (drive != INVALID_HANDLE_VALUE)
									{
										MessageBox(hWndParent, TEXT("Successfully formatted the new FAT partition!"), TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
										
										if (isFormat)
										{
											/* Write the "GATEWAYNAND" tag to the MBR in the SD card */
											cur_ptr = set_file_pointer(drive, 0, FILE_BEGIN);
											if (cur_ptr != -1)
											{
												dev_res = ReadFile(drive, buf, SECTOR_SIZE, (PDWORD)&res, NULL);
												if (dev_res && res == SECTOR_SIZE)
												{
													/* Replace the first 11 bytes in the buffer with the "GATEWAYNAND" tag */
													for (i = 0; i < TAG_LENGTH; i++) buf[i] = MAGIC_STR[0][i];
													
													/* Go back to sector #0 */
													cur_ptr = set_file_pointer(drive, 0, FILE_BEGIN);
													if (cur_ptr != -1)
													{
														/* Write the data back to the SD card */
														dev_res = WriteFile(drive, buf, SECTOR_SIZE, (PDWORD)&res, NULL);
														if (!dev_res || res != SECTOR_SIZE)
														{
															MessageBox(hWndParent, TEXT("Error writing \"GATEWAYNAND\" tag to the MBR."), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
															goto out;
														}
													} else {
														_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't set the file pointer to zero in \"%s\".", devname);
														MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
														goto out;
													}
												} else {
													_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't read %d bytes chunk from \"%s\" sector #0 (%d).", SECTOR_SIZE, devname, GetLastError());
													MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
													goto out;
												}
											} else {
												_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't set the file pointer to zero in \"%s\".", devname);
												MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
												goto out;
											}
										} else {
											/* Set file pointer to (nandsector - SECTOR_SIZE) and write the dummy header (only if we're not dealing with the first EmuNAND) */
											if (!write_dummy_data(drive, nandsector - SECTOR_SIZE)) goto out;
										}
									} else {
										MessageBox(hWndParent, TEXT("Couldn't reopen the handle to the physical drive!"), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
										goto out;
									}
								} else {
									_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't format the new FAT partition! (%d).", dev_res);
									MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
									goto out;
								}
							} else {
								MessageBox(hWndParent, TEXT("Couldn't modify the drive layout."), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
								goto out; 
							}
						}
					} else {
						_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Partition offset (0x%09llX) collides with the **%d%s** EmuNAND offset (0x%09llX)!", DriveLayout->PartitionEntry[0].StartingOffset.QuadPart, nandnum, NAND_NUM_STR(nandnum), nandsector);
						MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
						goto out;
					}
				} else {
#ifdef DEBUG_BUILD
					_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Partition already positioned beyond offset 0x%09llX.", fatsector - 1);
					if (is_input) wcscat(msg_info, L"\nSkipping drive layout modification procedure.");
					MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
				}
			} else {
				if (DriveLayout->PartitionEntry[0].StartingOffset.QuadPart < fatsector)
				{
					_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Partition offset (0x%09llX) collides with the **%d%s** EmuNAND offset (0x%09llX). This is probably some kind of corruption. Format the EmuNAND again to fix this.", DriveLayout->PartitionEntry[0].StartingOffset.QuadPart, nandnum, NAND_NUM_STR(nandnum), nandsector);
					MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
					goto out;
				} else {
#ifdef DEBUG_BUILD
					_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Partition already positioned beyond offset 0x%09llX.", fatsector - 1);
					MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
				}
			}
		} else {
			_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"The partition style used by the SD card (GPT) isn't supported.\nPlease reformat the card using the MBR partition style.");
			MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
			goto out;
		}
	} else {
		_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't get the drive layout (%d).", GetLastError());
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		goto out;
	}
	
	/* Write the EmuNAND name */
	if (!isFormat)
	{
		if (WriteReadNANDName(hWndParent, false) < 0) goto out;
	}
	
/*#ifdef DEBUG_BUILD
	_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"%s %d%s %sNAND %s the SD card, please wait...", (is_input ? L"Writing" : L"Reading"), nandnum, NAND_NUM_STR(nandnum), (cfw ? L"Red" : L"Emu"), (is_input ? L"to" : L"from"));
	MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif*/
	
	uint32_t cnt;
	uint8_t *nand_buf = malloc(NAND_BUF_SIZE);
	
	/* Set the progress bar range */
	SendMessage(ProgressBar, PBM_SETRANGE, 0, MAKELPARAM(0, ((real_nandsize > 0 ? real_nandsize : nandsize) / NAND_BUF_SIZE)));
	
	/* The real magic begins here */
	for (cnt = 0; cnt < nandsize; cnt += NAND_BUF_SIZE)
	{
		/* Set file pointer before doing any read/write operation */
		/* Remember to appropiately set the file pointer to the end of the NAND dump when dealing with the NCSD header (EmuNAND only) */
		offset = (cfw ? (nandsector + cnt) : (cnt > 0 ? (nandsector - SECTOR_SIZE + cnt) : (nandsector - SECTOR_SIZE + (real_nandsize > 0 ? real_nandsize : nandsize))));
		cur_ptr = set_file_pointer(drive, offset, FILE_BEGIN);
		if (cur_ptr == -1) break;
		
		if (is_input)
		{
			/* Fill buffer (file) */
			fread(nand_buf, NAND_BUF_SIZE, 1, nandfile);
			
			if (!cfw && cnt == 0)
			{
				/* Write the NCSD header contained in nand_buf */
				dev_res = WriteFile(drive, nand_buf, SECTOR_SIZE, (PDWORD)&res, NULL);
				if (!dev_res || res != SECTOR_SIZE) break;
				
				/* Go back to nandsector */
				cur_ptr = set_file_pointer(drive, nandsector, FILE_BEGIN);
				if (cur_ptr == -1) break;
				
				/* Write the rest of the buffer */
				dev_res = WriteFile(drive, &(nand_buf[SECTOR_SIZE]), NAND_BUF_SIZE - SECTOR_SIZE, (PDWORD)&res, NULL);
				if (!dev_res || res != (NAND_BUF_SIZE - SECTOR_SIZE)) break;
			} else {
				/* Write buffer (SD) */
				dev_res = WriteFile(drive, nand_buf, NAND_BUF_SIZE, (PDWORD)&res, NULL);
				if (!dev_res || res != NAND_BUF_SIZE) break;
			}
			
			/* Check if this is the last chunk */
			if (nandsize == (cnt + NAND_BUF_SIZE))
			{
				if (real_nandsize > 0)
				{
					/* Wipe the rest of the data from the previous NAND if its size was greater than the size of the NAND we just injected */
					
					uint8_t null_buf[NAND_BUF_SIZE] = {0};
					
					uint32_t diff_cnt, nand_diff = (real_nandsize - nandsize);
					
					for (diff_cnt = 0; diff_cnt < nand_diff; diff_cnt += NAND_BUF_SIZE)
					{
						offset = (cfw ? (nandsector + nandsize + diff_cnt) : (nandsector - SECTOR_SIZE + nandsize + diff_cnt));
						cur_ptr = set_file_pointer(drive, offset, FILE_BEGIN);
						if (cur_ptr == -1) break;
						
						dev_res = WriteFile(drive, null_buf, NAND_BUF_SIZE, (PDWORD)&res, NULL);
						if (!dev_res || res != NAND_BUF_SIZE) break;
						
						/* Update the progress bar */
						if (nand_diff > (diff_cnt + NAND_BUF_SIZE)) SendMessage(ProgressBar, PBM_STEPIT, 0, 0);
					}
					
					if (cur_ptr == -1 || !dev_res || res != NAND_BUF_SIZE) break;
				}
				
				/* Write the dummy footer */
				if (!write_dummy_data(drive, (cfw ? (offset + NAND_BUF_SIZE) : (offset + NAND_BUF_SIZE + SECTOR_SIZE)))) break;
			}
		} else {
			/* Fill buffer (SD) */
			dev_res = ReadFile(drive, nand_buf, NAND_BUF_SIZE, (PDWORD)&res, NULL);
			if (!dev_res || res != NAND_BUF_SIZE) break;
			
			if (!cfw && cnt == 0)
			{
				/* Go back to nandsector */
				cur_ptr = set_file_pointer(drive, nandsector, FILE_BEGIN);
				if (cur_ptr == -1) break;
				
				/* Replace the data after the first 512-bytes block in nand_buf */
				dev_res = ReadFile(drive, &(nand_buf[SECTOR_SIZE]), NAND_BUF_SIZE - SECTOR_SIZE, (PDWORD)&res, NULL);
				if (!dev_res || res != (NAND_BUF_SIZE - SECTOR_SIZE)) break;
			}
			
			/* Write buffer (file) */
			fwrite(nand_buf, 1, NAND_BUF_SIZE, nandfile);
			
			/* Flush buffer */
			fflush(nandfile);
		}
		
		/* Update the progress bar */
		SendMessage(ProgressBar, PBM_STEPIT, 0, 0);
	}
	
	free(nand_buf);
	
	if (cur_ptr == -1)
	{
		_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't seek to offset 0x%09llX in physical drive.", offset);
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
	} else
	if (!dev_res || (cnt == 0 && !cfw && res != (NAND_BUF_SIZE - SECTOR_SIZE)) || (cnt > 0 && res != NAND_BUF_SIZE))
	{
		_snwprintf(msg_info, GET_ARRAYSIZE(msg_info), L"Couldn't %s block #%u %s offset 0x%09llX.", (is_input ? L"write" : L"read"), cnt / NAND_BUF_SIZE, (is_input ? L"to" : L"from"), offset);
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
	} else {
		MessageBox(hWndParent, TEXT("Operation successfully completed!"), TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
	}
	
out:
	if (DriveLayout) free(DriveLayout);
	if (nandfile) fclose(nandfile);
	if (drive != INVALID_HANDLE_VALUE)
	{
		CloseHandle(drive);
		drive = INVALID_HANDLE_VALUE;
	}
}
