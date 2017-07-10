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

static wchar_t wc[512] = {0};
static wchar_t msg_info[512] = {0};

wchar_t *SingleDrive;
static wchar_t devname[30] = {0};
static HANDLE drive = INVALID_HANDLE_VALUE;

int format_volume(HWND hWndParent, wchar_t vol, char *VolId);

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
		_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"cur_ptr: 0x%08x%08x.", hi_ptr, lo_ptr);
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
			_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Wrote %d bytes long \"0x%04X\" dummy data at offset 0x%09llX.", SECTOR_SIZE, DUMMY_DATA, offset);
			MessageBox(NULL, msg_info, TEXT("Debug info"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif*/
			return true;
		} else {
			MessageBox(NULL, TEXT("Error writing dummy data."), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		}
	} else {
		_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't seek to offset 0x%09llX in physical drive.", offset);
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
		_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't get the disk geometry (%d).", GetLastError());
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
		return -1;
	}
	
	if (DiskGeometry.DiskSize.QuadPart > 0)
	{
/*#ifdef DEBUG_BUILD
		_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Drive capacity: %I64d bytes.", DiskGeometry.DiskSize.QuadPart);
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

int WriteReadNANDName(HWND hWndParent, bool read)
{
	int ret = 0;
	uint32_t index = GetDriveListIndex(false);
	bool is_open = (drive != INVALID_HANDLE_VALUE);
	
	/* Check if the current FAT layout is valid */
	if (MultiNANDDrives[index].fat_layout == 0)
	{
		if (!read)
		{
			_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"The FAT partition starting offset (0x%09llX) is not valid, even though it seems an EmuNAND was previously created in the selected SD card.\nBecause of this, it isn't possible to accurately calculate the offset where the EmuNAND is stored.\nThis is a very weird error. Remove the EmuNAND from your SD card and create it from scratch.", MultiNANDDrives[index].fat_offset);
			MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		}
		ret = -1;
		goto out;
	}
	
	/* Check if we're working with an EmuNAND that doesn't exist */
	if (nandnum > MultiNANDDrives[index].emunand_cnt)
	{
		snprintf(nand_name, MAX_CHARACTERS(nand_name), "EmuNAND #%d not available", nandnum);
		goto out;
	}
	
	if (!is_open)
	{
		/* Open physical drive */
		_snwprintf(devname, MAX_CHARACTERS(devname), L"\\\\.\\PhysicalDrive%u", MultiNANDDrives[index].drive_num);
		drive = CreateFile(devname, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (drive == INVALID_HANDLE_VALUE)
		{
			_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't open physical drive \"%s\" (%d).", devname, GetLastError());
			MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
			ret = -3;
			goto out;
		}
	}
	
	/* Generate the required offset */
	int64_t mbrsect = (MultiNANDDrives[index].fat_layout * (nandnum - 1));
	
	int64_t ptr = set_file_pointer(drive, mbrsect, FILE_BEGIN);
	if (ptr == -1)
	{
		_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't set the file pointer to sector #%d in drive %c:.", mbrsect / SECTOR_SIZE, MultiNANDDrives[index].drive_letter[0]);
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		ret = -4;
		goto out;
	}
	
	uint32_t res = 0;
	uint8_t buf[SECTOR_SIZE] = {0};
	int dev_res = ReadFile(drive, buf, SECTOR_SIZE, (PDWORD)&res, NULL);
	if (!dev_res || res != SECTOR_SIZE)
	{
		_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't read %d bytes chunk from sector #%d in drive %c: (%d).", SECTOR_SIZE, mbrsect / SECTOR_SIZE, MultiNANDDrives[index].drive_letter[0], GetLastError());
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		ret = -5;
		goto out;
	}
	
	if (memcmp(buf + 16, &(MAGIC_STR[3][0]), TAG_LENGTH) == 0) // "EMUNAND9SD "
	{
		// Disable name reading/writing. EmuNAND9 isn't compatible with this feature and we may wipe the info it stores in the MBR if we proceed
		snprintf(nand_name, MAX_CHARACTERS(nand_name), "Not compatible with EmuNAND9");
	} else {
		if (read)
		{
			if (memcmp(buf + 11, "NAME", 4) == 0)
			{
				strncpy(nand_name, (char*)(buf + 15), NAME_LENGTH - 1);
			} else {
				snprintf(nand_name, MAX_CHARACTERS(nand_name), "[NO NAME]");
			}
		} else {
			memcpy(buf + 11, "NAME", 4);
			memset(buf + 15, 0x00, NAME_LENGTH);
			strncpy((char*)(buf + 15), nand_name, strlen(nand_name));
			
			/* Go back to the previous sector */
			ptr = set_file_pointer(drive, mbrsect, FILE_BEGIN);
			if (ptr == -1)
			{
				_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't set the file pointer to sector #%u in \"%s\".", mbrsect / SECTOR_SIZE, devname);
				MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
				ret = -4;
				goto out;
			}
			
			dev_res = WriteFile(drive, buf, SECTOR_SIZE, (PDWORD)&res, NULL);
			if (!dev_res || res != SECTOR_SIZE)
			{
				_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't write %d bytes chunk to \"%s\" sector #%u (%d).", SECTOR_SIZE, devname, mbrsect / SECTOR_SIZE, GetLastError());
				MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
				ret = -6;
			} else {
				if (!is_open)
				{
					_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Successfully wrote the %d%s EmuNAND name!", nandnum, NAND_NUM_STR(nandnum));
					MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
				}
			}
		}
	}
	
out:
	if (!is_open)
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

int ParseEmuNANDs(HWND hWndParent, uint32_t index)
{
	/* No point in doing this if we haven't opened a drive */
	if (drive == INVALID_HANDLE_VALUE) return -1;
	
	int i, j, ret = 0;
	int64_t offset = 0;
	bool is_n3ds, found = false;
	uint8_t buf[SECTOR_SIZE] = {0};
	uint32_t nandsize;
	int8_t nand_cnt = 0;
	
	int64_t o3ds_nand_offsets[3] = { SECTOR_SIZE, O3DS_TOSHIBA_NAND, O3DS_SAMSUNG_NAND };
	int64_t o3ds_fat_offsets[3] = { O3DS_LEGACY_FAT, O3DS_DEFAULT_FAT, O3DS_MINIMUM_FAT };
	
	int64_t n3ds_nand_offsets[3] = { SECTOR_SIZE, N3DS_SAMSUNG_NAND, N3DS_UNKNOWN_NAND };
	int64_t n3ds_fat_offsets[3] = { N3DS_LEGACY_FAT, N3DS_DEFAULT_FAT, N3DS_MINIMUM_FAT };
	
	/* Check if the FAT partition offset is valid */
	for (i = 0; i < 3; i++)
	{
		for (j = 1; j <= MAX_NAND_NUM; j++)
		{
			if (MultiNANDDrives[index].fat_offset == (o3ds_fat_offsets[i] * j))
			{
				is_n3ds = false;
				found = true;
				break;
			}
			
			if (MultiNANDDrives[index].fat_offset == (n3ds_fat_offsets[i] * j))
			{
				is_n3ds = true;
				found = true;
				break;
			}
		}
		
		if (found) break;
	}
	
	if (!found)
	{
		/* This is very weird. Nonetheless, we will use this value as a reference in InjectExtractNAND() */
		MultiNANDDrives[index].fat_layout = 0;
		return -1;
	} else {
		/* Update the FAT setup entry in the DRIVE_INFO struct. This will serve as a reference for the creation of additional EmuNANDs */
		MultiNANDDrives[index].fat_layout = (is_n3ds ? n3ds_fat_offsets[i] : o3ds_fat_offsets[i]);
	}
	
	for (i = 0; i < MAX_NAND_NUM; i++)
	{
		/* Check if the first EmuNAND is an O3DS Toshiba EmuNAND, O3DS Samsung EmuNAND, N3DS Samsung EmuNAND, N3DS **Unknown** EmuNAND or a RedNAND (O3DS / N3DS) */
		/* Further iterations perform the same check for additional EmuNANDs, but will only be executed if the previous EmuNAND exists */
		/* It should also be noted that the calculations for the additional EmuNAND offsets will be based on the partition style used by the first EmuNAND */
		
		/* Depending on the type of the NAND stored in the SD card, the data has to be read in different ways */
		/* This is because the order in which the data is written varies between each type of NAND */
		
		/* EmuNAND: the first 0x200 bytes (NCSD header) are stored **after** the NAND dump. The NAND dump starts to be written */
		/*			from offset 0x200 to the SD card. Used by Gateway and modern CFWs, like rxTools, CakesFW and ReiNand */
		
		/* RedNAND: first used by the Palantine CFW, now compatible with more CFWs. It is written to the SD card 'as is', beginning */
		/*			with the NCSD header and following with the rest of the NAND data */
		
		/* Usually, a dummy footer follows afterwards. This applies for both types of NANDs, and serves to indicate the appropiate */
		/* NAND flash capacity of the 3DS console */
		
		found = false;
		
		for (j = 0; j < 3; j++)
		{
			offset = (is_n3ds ? (n3ds_nand_offsets[j] + (MultiNANDDrives[index].fat_offset * i)) : (o3ds_nand_offsets[j] + (MultiNANDDrives[index].fat_offset * i)));
			ret = CheckHeader(index, offset, buf, false);
			if (ret == 0)
			{
				found = true;
				nand_cnt++;
				break;
			} else
			if (ret == -1)
			{
#ifdef DEBUG_BUILD
				_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Unable to locate the NCSD header at offset 0x%09llX.", offset);
				MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
			} else {
				break;
			}
		}
		
		if (!found || ret < -1) break;
		
		if (found)
		{
			/* Check if we're dealing with a RedNAND */
			if (j == 0)
			{
				/* Calculate NAND dump size (based in the dummy footer position) */
				/* This steps will only be performed on a RedNAND */
				int k;
				int64_t footer_offset;
				uint8_t dummy_buf[SECTOR_SIZE] = {0};
				
				for (k = 1; k < 3; k++)
				{
					/* Old 3DS: Check if this a Toshiba RedNAND (k == 1) or a Samsung RedNAND (k == 2), in that order */
					/* New 3DS: Check if this a Samsung RedNAND (k == 1) or an **Unknown** RedNAND (k == 2), in that order */
					footer_offset = (is_n3ds ? (offset + n3ds_nand_offsets[k]) : (offset + o3ds_nand_offsets[k]));
					ret = CheckHeader(index, footer_offset, dummy_buf, true);
					if (ret == 0 || ret < -1) break;
				}
				
				if (ret == 0)
				{
					/* We hit the sweet spot */
					nandsize = (is_n3ds ? (uint32_t)n3ds_nand_offsets[k] : (uint32_t)o3ds_nand_offsets[k]);
				} else
				if (ret < -1)
				{
					break;
				} else {
#ifdef DEBUG_BUILD
					MessageBox(hWndParent, TEXT("Dummy footer not available in the SD card.\nRedNAND size will be calculated based in the NCSD header info.\nIt may not match your actual NAND flash capacity."), TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
					
					/* RedNAND size calculation procedure (based in the NCSD header info) */
					/* May not match actual NAND flash capacity */
					nandsize = 0;
					
					for (k = 0x124; k < 0x160; k += 8)
					{
						uint32_t partition_len = 0;
						memcpy(&partition_len, &(buf[k]), 4);
						nandsize += (partition_len * MEDIA_UNIT_SIZE);
					}
				}
			} else {
				/* No need to calculate what we already know */
				nandsize = (is_n3ds ? (uint32_t)n3ds_nand_offsets[j] : (uint32_t)o3ds_nand_offsets[j]);
			}
			
			/* Update the DRIVE_INFO struct */
			MultiNANDDrives[index].emunand_sizes[i] = nandsize;
			MultiNANDDrives[index].rednand[i] = (j == 0);
			
#ifdef DEBUG_BUILD
			_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Valid NCSD header detected at offset 0x%09llX.\nConsole: %ls 3DS.\nNAND number: %d.\nNAND type: %ls.\nNAND size: %u bytes.\nFAT layout: %ls.", offset, is_n3ds ? L"New" : L"Old", i + 1, (j == 0 ? L"RedNAND" : (j == 1 ? (is_n3ds ? L"Samsung" : L"Toshiba") : (is_n3ds ? L"**Unknown**" : L"Samsung"))), nandsize, FAT_LAYOUT_STR(MultiNANDDrives[index].fat_layout));
			MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
		}
	}
	
	if (ret == -2)
	{
		_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't seek to offset 0x%09llX in physical drive.", offset);
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		return -1;
	}
	
	if (ret == -3)
	{
		_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't read %d bytes block from offset 0x%09llX.", SECTOR_SIZE, offset);
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		return -1;
	}
	
	/* Update the N3DS status and the EmuNAND count in the DRIVE_INFO struct */
	if (nand_cnt > 0)
	{
		MultiNANDDrives[index].n3ds = is_n3ds;
		MultiNANDDrives[index].emunand_cnt = nand_cnt;
	}
	
	return 0;
}

int ParseDrives(HWND hWndParent)
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
		/* No problem, though */
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
					if (res == DRIVE_REMOVABLE || res == DRIVE_FIXED)
					{
						/* Open logical drive */
						_snwprintf(devname, MAX_CHARACTERS(devname), L"\\\\.\\%c:", SingleDrive[0]);
						drive = CreateFile(devname, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
						if (drive != INVALID_HANDLE_VALUE)
						{
							/* Check if the drive is ready */
							dev_res = DeviceIoControl(drive, IOCTL_STORAGE_CHECK_VERIFY, NULL, 0, NULL, 0, (PDWORD)&res, NULL);
							if (dev_res != 0)
							{
								VOLUME_DISK_EXTENTS diskExtents;
								dev_res = DeviceIoControl(drive, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, (void*)&diskExtents, (uint32_t)sizeof(diskExtents), (PDWORD)&res, NULL);
								
								CloseHandle(drive);
								drive = INVALID_HANDLE_VALUE;
								
								if (dev_res && res > 0)
								{
									/* Then again, we don't want to run the code on drive #0 */
									/* This may be an additional partition placed after C: */
									if (diskExtents.Extents[0].DiskNumber > 0)
									{
										/* Open physical drive */
										_snwprintf(devname, MAX_CHARACTERS(devname), L"\\\\.\\PhysicalDrive%u", (unsigned int)diskExtents.Extents[0].DiskNumber);
										drive = CreateFile(devname, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
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
														if (!contains_tag)
														{
															_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"MBR signature (logical drive \"%c:\"):\n", SingleDrive[0]);
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
															_snwprintf(MultiNANDDrives[j].drive_letter, MAX_CHARACTERS(MultiNANDDrives[j].drive_letter), L"%c:\\", SingleDrive[0]);
															MultiNANDDrives[j].drive_num = (unsigned int)diskExtents.Extents[0].DiskNumber;
															MultiNANDDrives[j].drive_sz = drive_sz;
															
															uint32_t fat_offset = 0;
															memcpy(&fat_offset, &(buf[0x1C6]), 4);
															MultiNANDDrives[j].fat_offset = ((int64_t)fat_offset * SECTOR_SIZE);
															
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
														_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't read %d bytes chunk from \"%s\" sector #0 (%d).", SECTOR_SIZE, devname, GetLastError());
														MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
													}
												} else {
#ifdef DEBUG_BUILD
													_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't set the file pointer to zero in \"%s\".", devname);
													MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
												}
											} else {
#ifdef DEBUG_BUILD
												if (drive_sz <= 0)
												{
													_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't get the drive capacity for \"%s\".", devname);
													MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
												}
#endif
											}
										} else {
#ifdef DEBUG_BUILD
											_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't open physical drive \"%s\" (%d).", devname, GetLastError());
											MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
										}
									}
								} else {
#ifdef DEBUG_BUILD
									_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't retrieve disk volume information for \"%s\".\ndev_res: %d / res: %u.", devname, dev_res, res);
									MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
								}
							} else {
#ifdef DEBUG_BUILD
								_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Logical drive \"%c:\" not ready (empty drive?).", SingleDrive[0]);
								MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
							}
						} else {
#ifdef DEBUG_BUILD
							_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't open logical drive \"%s\" (%d).", devname, GetLastError());
							MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
						}
					} else {
#ifdef DEBUG_BUILD
						if (res == DRIVE_UNKNOWN)
						{
							_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Unknown drive type for \"%c:\".", SingleDrive[0]);
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
	} else {
		if (MultiNANDDrives) free(MultiNANDDrives);
	}
	
#ifdef DEBUG_BUILD
	if (j > 0)
	{
		for (i = 0; i < j; i++)
		{
			_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Drive #%d info:\n\n- String: \"%ls\".\n- Letter: %ls.\n- Number: %u.\n- Capacity: %I64d bytes.\n- Partition offset: 0x%09llX.", i + 1, MultiNANDDrives[i].drive_str, MultiNANDDrives[i].drive_letter, MultiNANDDrives[i].drive_num, MultiNANDDrives[i].drive_sz, MultiNANDDrives[i].fat_offset);
			MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
		}
	}
#endif
	
	return 0;
}

void InjectExtractNAND(wchar_t *fname, HWND hWndParent, bool isFormat)
{
	bool n3ds;
	int i, dev_res;
	FILE *nandfile = NULL;
	uint8_t buf[SECTOR_SIZE] = {0};
	int64_t cur_ptr = -1, fatsector = 0, nandsector = 0, offset = 0;
	uint32_t res, index = GetDriveListIndex(isFormat), nandsize = 0, magic_word = 0;
	
	uint32_t DriveLayoutLen = sizeof(DRIVE_LAYOUT_INFORMATION_EX) + (3 * sizeof(PARTITION_INFORMATION_EX));
	DRIVE_LAYOUT_INFORMATION_EX *DriveLayout = malloc(DriveLayoutLen);
	
	/* Check if the current FAT layout is valid */
	if (!isFormat && MultiNANDDrives[index].fat_layout == 0)
	{
		_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"The FAT partition starting offset (0x%09llX) is not valid, even though it seems an EmuNAND was previously created in the selected SD card.\nBecause of this, it isn't possible to accurately calculate the offset where the EmuNAND is stored.\nThis is a very weird error. Remove the EmuNAND from your SD card and create it from scratch.", MultiNANDDrives[index].fat_offset);
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		goto out;
	}
	
	/* Open 3DS NAND dump */
	nandfile = _wfopen(fname, (is_input ? L"rb" : L"wb"));
	if (!nandfile)
	{
		_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't open \"%s\" for %s.", fname, (is_input ? L"reading" : L"writing"));
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		goto out;
	}
	
	if (is_input)
	{
		/* Store NAND dump size */
		fseek(nandfile, 0, SEEK_END);
		nandsize = ftell(nandfile);
		rewind(nandfile);
		
		n3ds = (nandsize == N3DS_SAMSUNG_NAND || nandsize == N3DS_UNKNOWN_NAND);
		if ((nandsize != O3DS_TOSHIBA_NAND && nandsize != (O3DS_TOSHIBA_NAND + SECTOR_SIZE) && nandsize != O3DS_SAMSUNG_NAND && nandsize != (O3DS_SAMSUNG_NAND + SECTOR_SIZE) && nandsize != N3DS_SAMSUNG_NAND && nandsize != N3DS_UNKNOWN_NAND) || (!isFormat && n3ds != MultiNANDDrives[index].n3ds) || (!isFormat && nandsize > MultiNANDDrives[index].emunand_sizes[nandnum - 1]))
		{
			_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Invalid 3DS NAND dump.\nFilesize (%u bytes) is invalid.", nandsize);
			MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
			goto out;
		}
		
		bool is_rednand = (nandsize == (O3DS_TOSHIBA_NAND + SECTOR_SIZE) || nandsize == (O3DS_SAMSUNG_NAND + SECTOR_SIZE));
		if (is_rednand && !cfw)
		{
			/* Warn the user about the use of an input RedNAND */
			dev_res = MessageBox(hWndParent, TEXT("The selected input NAND dump was previously patched with the \"drag_emunand_here\" batch script, and therefore, is a RedNAND.\n\nDo you want to inject this file as a RedNAND?\nIf you select \"No\", the NAND dump will be written as a common EmuNAND."), TEXT("Warning"), MB_ICONWARNING | MB_YESNO | MB_SETFOREGROUND);
			if (dev_res == IDYES)
			{
				/*  Override configuration */
				cfw = true;
			}
		}
		
#ifdef DEBUG_BUILD
		_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"%s 3DS %s NAND dump detected!\nFilesize: %u bytes.", (!n3ds ? L"Old" : L"New"), NAND_TYPE_STR(nandsize), nandsize);
		MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
		
		/* Check if the supplied NAND dump does contain an NCSD header */
		fseek(nandfile, (is_rednand ? (SECTOR_SIZE + 0x100) : 0x100), SEEK_SET);
		fread(&magic_word, 4, 1, nandfile);
		rewind(nandfile);
		
		if (magic_word == bswap_32(NCSD_MAGIC))
		{
#ifdef DEBUG_BUILD
			_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Valid NCSD header detected at offset 0x%08x.", (is_rednand ? SECTOR_SIZE : 0));
			MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
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
	} else {
		/* Adjust New 3DS status */
		n3ds = MultiNANDDrives[index].n3ds;
		
		/* Adjust NAND size */
		nandsize = (uint32_t)MultiNANDDrives[index].emunand_sizes[nandnum - 1];
		
		/* Adjust RedNAND configuration */
		if (MultiNANDDrives[index].rednand[nandnum - 1]) cfw = true;
	}
	
	if (!isFormat)
	{
		/* Generate required offsets */
		fatsector = ((MultiNANDDrives[index].fat_layout) * nandnum);
		nandsector = (SECTOR_SIZE + (MultiNANDDrives[index].fat_layout * (nandnum - 1)));
		
		/* Check if we're trying to extract an EmuNAND that doesn't exist */
		if (nandnum > MultiNANDDrives[index].emunand_cnt)
		{
			if (!is_input)
			{
#ifdef DEBUG_BUILD
				_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Drive %c: doesn't contain a %d%s EmuNAND!", MultiNANDDrives[index].drive_letter[0], nandnum, NAND_NUM_STR(nandnum));
				MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
				goto out;
			}
			
			/* Check storage capacity */
			if (MultiNANDDrives[index].drive_sz <= fatsector)
			{
				_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Your SD card must have a capacity of at least %u GiB to store the %d%s EmuNAND.", CAPACITY(nandnum, n3ds), nandnum, NAND_NUM_STR(nandnum));
				MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
				goto out;
			}
		}
	} else {
		/* Generate required offsets */
		fatsector = (!n3ds ? ((int64_t)O3DS_DEFAULT_FAT * nandnum) : ((int64_t)N3DS_DEFAULT_FAT * nandnum));
		nandsector = (!n3ds ? (SECTOR_SIZE + ((int64_t)O3DS_DEFAULT_FAT * (nandnum - 1))) : (SECTOR_SIZE + ((int64_t)N3DS_DEFAULT_FAT * (nandnum - 1))));
		
		/* Check storage capacity */
		if (MultiNANDDrives[index].drive_sz <= fatsector)
		{
			_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Your SD card must have a capacity of at least %u GiB to store the %d%s EmuNAND.", CAPACITY(nandnum, n3ds), nandnum, NAND_NUM_STR(nandnum));
			MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
			goto out;
		}
	}
	
	/* Open physical drive */
	_snwprintf(devname, MAX_CHARACTERS(devname), L"\\\\.\\PhysicalDrive%u", MultiNANDDrives[index].drive_num);
	drive = CreateFile(devname, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (drive == INVALID_HANDLE_VALUE)
	{
		_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't open physical drive \"%s\" (%d).", devname, GetLastError());
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		goto out;
	}
	
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
			char VolId[12] = {0};
			
			if (!isFormat && DriveLayout->PartitionStyle == PARTITION_STYLE_MBR && DriveLayout->PartitionEntry[0].Mbr.PartitionType != PARTITION_FAT32 && DriveLayout->PartitionEntry[0].Mbr.PartitionType != PARTITION_FAT32_LBA && DriveLayout->PartitionEntry[0].Mbr.PartitionType != PARTITION_FAT_16 && DriveLayout->PartitionEntry[0].Mbr.PartitionType != PARTITION_FAT16B)
			{
				_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Detected partition type: 0x%02X.\nYour SD card isn't currently using a FAT partition.\nThis is weird, considering that the SD card already contains an EmuNAND.\nThis means it will *not* work properly with your 3DS console.\n\n", DriveLayout->PartitionEntry[0].Mbr.PartitionType);
				
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
						
						snprintf(VolId, MAX_CHARACTERS(VolId), "%.11s", (char*)(&(MAGIC_STR[0][0]))); // Always use the GATEWAYNAND tag
						
						/* Format the new partition */
						dev_res = format_volume(hWndParent, MultiNANDDrives[index].drive_letter[0], VolId);
						if (dev_res == 0)
						{
							/* Reopen the handle to the physical drive */
							drive = CreateFile(devname, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
							if (drive != INVALID_HANDLE_VALUE)
							{
								/* Get drive layout */
								dev_res = DeviceIoControl(drive, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, DriveLayout, DriveLayoutLen, (PDWORD)&res, NULL);
								if (dev_res)
								{
									MessageBox(hWndParent, TEXT("Successfully formatted the new FAT partition!"), TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
								} else {
									_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't get the drive layout (%d).", GetLastError());
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
				if (DriveLayout->PartitionEntry[0].StartingOffset.QuadPart < fatsector)
				{
					if (is_input)
					{
						/* Move filesystem to the right */
						_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"FAT partition detected below the required offset (at 0x%09llX).\nDo you want to begin the drive layout modification procedure?\n\nPlease, bear in mind that doing so will wipe the data on your current FAT partition.\nIf you select \"No\", the process will be canceled.", DriveLayout->PartitionEntry[0].StartingOffset.QuadPart);
						dev_res = MessageBox(hWndParent, msg_info, TEXT("Warning"), MB_ICONWARNING | MB_YESNO | MB_SETFOREGROUND);
						if (dev_res == IDNO)
						{
							goto out;
						} else {
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
								_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"FAT partition successfully moved to offset 0x%09llX!", fatsector);
								MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
								CloseHandle(drive);
								drive = INVALID_HANDLE_VALUE;
								
								snprintf(VolId, MAX_CHARACTERS(VolId), "%.11s", (char*)(&(MAGIC_STR[0][0]))); // Always use the GATEWAYNAND tag
								
								/* Format the new partition */
								dev_res = format_volume(hWndParent, MultiNANDDrives[index].drive_letter[0], VolId);
								if (dev_res == 0)
								{
									/* Reopen the handle to the physical drive */
									drive = CreateFile(devname, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
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
														_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't set the file pointer to zero in \"%s\".", devname);
														MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
														goto out;
													}
												} else {
													_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't read %d bytes chunk from \"%s\" sector #0 (%d).", SECTOR_SIZE, devname, GetLastError());
													MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
													goto out;
												}
											} else {
												_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't set the file pointer to zero in \"%s\".", devname);
												MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
												goto out;
											}
										} else {
											/* Set file pointer to (nandsector - SECTOR_SIZE) and write the dummy header */
											if (!write_dummy_data(drive, nandsector - SECTOR_SIZE)) goto out;
										}
									} else {
										MessageBox(hWndParent, TEXT("Couldn't reopen the handle to the physical drive!"), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
										goto out;
									}
								} else {
									_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't format the new FAT partition! (%d).", dev_res);
									MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
									goto out;
								}
							} else {
								MessageBox(hWndParent, TEXT("Couldn't modify the drive layout."), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
								goto out; 
							}
						}
					} else {
						_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"FAT partition offset (0x%09llX) collides with the **%d%s** EmuNAND offset (0x%09llX). The %s %c GiB segment after 0x%09llX hasn't been created!", DriveLayout->PartitionEntry[0].StartingOffset.QuadPart, nandnum, NAND_NUM_STR(nandnum), nandsector, NAND_NUM_STR(nandnum), nandsector - SECTOR_SIZE - 1);
						MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
						goto out;
					}
				} else {
#ifdef DEBUG_BUILD
					_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"FAT partition already positioned beyond offset 0x%09llX.", fatsector - 1);
					if (is_input) wcscat(msg_info, L"\nSkipping drive layout modification procedure.");
					MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
				}
			} else {
				if (DriveLayout->PartitionEntry[0].StartingOffset.QuadPart < fatsector)
				{
					_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"FAT partition offset (0x%09llX) collides with the **%d%s** EmuNAND offset (0x200). This is probably some kind of corruption. Format the EmuNAND again to fix this.", DriveLayout->PartitionEntry[0].StartingOffset.QuadPart, nandnum, NAND_NUM_STR(nandnum));
					MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
					goto out;
				} else {
#ifdef DEBUG_BUILD
					_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"FAT partition already positioned beyond offset 0x%09llX.", fatsector - 1);
					MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
				}
			}
		} else {
			_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"The partition style used by the SD card (GPT) isn't supported.\nPlease reformat the card using the MBR partition style.");
			MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
			goto out;
		}
	} else {
		_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't get the drive layout (%d).", GetLastError());
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		goto out;
	}
	
	/* Write the EmuNAND name */
	if (!isFormat)
	{
		if (WriteReadNANDName(hWndParent, false) < 0) goto out;
	}
	
/*#ifdef DEBUG_BUILD
	_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"%s %d%s %sNAND %s the SD card, please wait...", (is_input ? L"Writing" : L"Reading"), nandnum, NAND_NUM_STR(nandnum), (cfw ? L"Red" : L"Emu"), (is_input ? L"to" : L"from"));
	MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif*/
	
	uint32_t cnt;
	uint8_t *nand_buf = malloc(NAND_BUF_SIZE);
	
	/* Set the progress bar range */
	SendMessage(ProgressBar, PBM_SETRANGE, 0, MAKELPARAM(0, (nandsize / NAND_BUF_SIZE)));
	
	/* The real magic begins here */
	for (cnt = 0; cnt < nandsize; cnt += NAND_BUF_SIZE)
	{
		/* Set file pointer before doing any read/write operation */
		/* Remember to appropiately set the file pointer to the end of the NAND dump when dealing with the NCSD header (EmuNAND only) */
		offset = (cfw ? (nandsector + cnt) : (cnt > 0 ? (nandsector - SECTOR_SIZE + cnt) : (nandsector - SECTOR_SIZE + nandsize)));
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
				
				/* Go back to sector #1 */
				cur_ptr = set_file_pointer(drive, SECTOR_SIZE, FILE_BEGIN);
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
			if (nandsize == cnt + NAND_BUF_SIZE)
			{
				/* Write the dummy footer */
				if (!write_dummy_data(drive, (cfw ? (offset + NAND_BUF_SIZE) : (offset + NAND_BUF_SIZE + SECTOR_SIZE)))) break;
			}
		} else {
			/* Fill buffer (SD) */
			dev_res = ReadFile(drive, nand_buf, NAND_BUF_SIZE, (PDWORD)&res, NULL);
			if (!dev_res || res != NAND_BUF_SIZE) break;
			
			if (!cfw && cnt == 0)
			{
				/* Go back to sector #1 */
				cur_ptr = set_file_pointer(drive, SECTOR_SIZE, FILE_BEGIN);
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
		_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't seek to offset 0x%09llX in physical drive.", offset);
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		goto out;
	} else
	if (!dev_res || (cnt == 0 && !cfw && res != (NAND_BUF_SIZE - SECTOR_SIZE)) || (cnt > 0 && res != NAND_BUF_SIZE))
	{
		_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't %s block #%u %s offset 0x%09llX.", (is_input ? L"write" : L"read"), cnt / NAND_BUF_SIZE, (is_input ? L"to" : L"from"), offset);
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		goto out;
	}
	
	MessageBox(hWndParent, TEXT("Operation successfully completed!"), TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
	
out:
	if (DriveLayout) free(DriveLayout);
	if (nandfile) fclose(nandfile);
	if (drive != INVALID_HANDLE_VALUE)
	{
		CloseHandle(drive);
		drive = INVALID_HANDLE_VALUE;
	}
}
