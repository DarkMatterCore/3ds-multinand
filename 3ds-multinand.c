#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <windows.h>
#include <winioctl.h>

#define VERSION "0.7"
#define BAR_LEN 50

#define SECTOR_SIZE			512
#define MEDIA_UNIT_SIZE		SECTOR_SIZE
#define NAND_BUF_SIZE		(SECTOR_SIZE * 64)					// 32 KiB

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
#define PARTITION_FAT16		0x06

#define MAX_CHARACTERS(x)	((sizeof((x))) / (sizeof((x)[0])))	// Returns the number of elements in an array
#define NAND_NUM_STR(x)		((x) == 1 ? "first" : ((x) == 2 ? "second" : ((x) == 3 ? "third" : "fourth")))
#define NAND_TYPE_STR(x)	(((x) == TOSHIBA_NAND || (x) == TOSHIBA_REDNAND) ? "Toshiba" : (((x) == SAMSUNG_NAND || (x) == SAMSUNG_REDNAND || (x) == N3DS_SAMSUNG_NAND) ? "Samsung" : "**Unknown**"))

#define PTR_HIGH(x)			((int32_t)((x) >> 32))
#define PTR_LOW(x)			((int32_t)(x))
#define PTR_FULL(x,y)		(((int64_t)(x) << 32) | (y))
#define bswap_16(a)			((((a) << 8) & 0xff00) | (((a) >> 8) & 0xff))
#define bswap_32(a)			((((a) << 24) & 0xff000000) | (((a) << 8) & 0xff0000) | (((a) >> 8) & 0xff00) | (((a) >> 24) & 0xff))

/* To do: add compatibility with strings from more flashcards */
const uint8_t MAGIC_STR[3][11] = {	{ 0x47, 0x41, 0x54, 0x45, 0x57, 0x41, 0x59, 0x4E, 0x41, 0x4E, 0x44 }, // "GATEWAYNAND"
									{ 0x4D, 0x54, 0x43, 0x41, 0x52, 0x44, 0x5F, 0x4E, 0x41, 0x4E, 0x44 }, // "MTCARD_NAND"
									{ 0x33, 0x44, 0x53, 0x43, 0x41, 0x52, 0x44, 0x4E, 0x41, 0x4E, 0x44 }  // "3DSCARDNAND"
								 };

/* Function prototypes */
bool IsUserAdmin(void);
void print_info();
int64_t set_file_pointer(HANDLE h, int64_t new_ptr, uint32_t method);
bool write_dummy_data(HANDLE SDcard, int64_t offset, bool verbose);
int64_t get_drive_size(HANDLE h);
void print_progress(uint32_t read_amount, uint32_t fullsize);
void print_divider_bar();

int main(int argc, char **argv)
{
	printf("\n\t3DS Multi EmuNAND Creator v%s - By DarkMatterCore\n", VERSION);
	
	if (argc < 5 || argc > 6 || (strncmp(argv[1], "-old", 4) != 0 && strncmp(argv[1], "-new", 4) != 0) || (strncmp(argv[2], "-1", 2) != 0 && strncmp(argv[2], "-2", 2) != 0 && strncmp(argv[2], "-3", 2) != 0 && strncmp(argv[2], "-4", 2) != 0) || (strncmp(argv[3], "-i", 2) != 0 && strncmp(argv[3], "-o", 2) != 0 && strncmp(argv[3], "-cfw", 4) != 0))
	{
		print_info();
		return 1;
	}
	
	int i, dev_res;
	uint32_t drivenum;
	int64_t cur_ptr = -1;
	FILE *nandfile = NULL;
	char devname[30] = {0};
	uint8_t buf[SECTOR_SIZE] = {0};
	HANDLE drive = INVALID_HANDLE_VALUE;
	
	uint32_t DriveLayoutLen = sizeof(DRIVE_LAYOUT_INFORMATION_EX) + (3 * sizeof(PARTITION_INFORMATION_EX));
	DRIVE_LAYOUT_INFORMATION_EX *DriveLayout = malloc(DriveLayoutLen);
	
	bool is_new = (strncmp(argv[1], "-new", 4) == 0);
	bool input_mode = (strncmp(argv[3], "-i", 2) == 0 || strncmp(argv[3], "-cfw", 4) == 0);
	bool cfw = (!is_new && strncmp(argv[3], "-cfw", 4) == 0);
	
	uint8_t mbr_str = 0;
	int8_t nandnum = (argv[2][1] - 0x30); // ASCII to base-10 integer conversion
	int64_t fatsector = (!is_new ? ((int64_t)O3DS_FS_BASE_SECTOR * nandnum) : ((int64_t)N3DS_FS_BASE_SECTOR * nandnum));
	int64_t nandsect = (!is_new ? (SECTOR_SIZE + ((int64_t)O3DS_FS_BASE_SECTOR * (nandnum - 1))) : (SECTOR_SIZE + ((int64_t)N3DS_FS_BASE_SECTOR * (nandnum - 1))));
	
	/* Check if we have administrative privileges */
	if (!IsUserAdmin())
	{
		printf("\n\tError: Not running with administrative privileges.\n");
		printf("\tPlease make sure you run the program as an Admin and try again.\n");
		goto boot_bin;
	}
	
	/* Get the total amount of available logical drives */
	char *SingleDrive;
	char LogicalDrives[MAX_PATH] = {0};
	uint32_t res = GetLogicalDriveStrings(MAX_PATH, LogicalDrives);
	if (res > 0 && res <= MAX_PATH)
	{
		/* Try to open each logical drive through CreateFile(), in order to get their physical drive number */
		/* This is because we won't operate on a filesystem level */
		/* Once we get the physical drive number, we'll close the current handle and try to open the disk as a physical media */
		SingleDrive = LogicalDrives;
		while (*SingleDrive)
		{
			/* Skip drives A:, B: and C: to avoid problems */
			if (SingleDrive[0] != 'A' && SingleDrive[0] != 'B' && SingleDrive[0] != 'C')
			{
				/* Get the drive type */
				res = GetDriveType(SingleDrive);
				if (res == DRIVE_REMOVABLE || res == DRIVE_FIXED)
				{
					/* Open logical drive */
					snprintf(devname, MAX_CHARACTERS(devname), "\\\\.\\%c:", SingleDrive[0]);
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
									snprintf(devname, MAX_CHARACTERS(devname), "\\\\.\\PhysicalDrive%u", (unsigned int)diskExtents.Extents[0].DiskNumber);
									drive = CreateFile(devname, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
									if (drive != INVALID_HANDLE_VALUE)
									{
										/* Check if this is actually the SD card that contains the EmuNAND */
										/* Just for safety, let's set the file pointer to zero before attempting to read */
										cur_ptr = set_file_pointer(drive, 0, FILE_BEGIN);
										if (cur_ptr != -1)
										{
											/* Read operations have to be aligned to 512 bytes in order to get this to work */
											dev_res = ReadFile(drive, buf, SECTOR_SIZE, (PDWORD)&res, NULL);
											if (dev_res && res == SECTOR_SIZE)
											{
												for (i = 0; i < 3; i++)
												{
													if (memcmp(buf, &(MAGIC_STR[i][0]), 11) == 0)
													{
														/* Found it! */
														mbr_str = (i + 1);
														drivenum = diskExtents.Extents[0].DiskNumber;
														break;
													}
												}
												
												if (mbr_str > 0)
												{
													break;
												} /*else {
													printf("\n\tMBR signature (logical drive \"%c:\"): ", SingleDrive[0]);
													for (i = 0; i < 11; i++) printf("%02X ", buf[i]);
													printf("\n");
												}*/
											} else {
												printf("\n\tError reading %d bytes chunk from \"%s\" sector #0 (%d).\n", SECTOR_SIZE, devname, GetLastError());
											}
										} else {
											printf("\n\tError setting file pointer to zero in \"%s\".\n", devname);
										}
										
										CloseHandle(drive);
										drive = INVALID_HANDLE_VALUE;
									} else {
										printf("\n\tError opening physical drive \"%s\" (%d).\n", devname, GetLastError());
									}
								}
							} else {
								printf("\n\tError retrieving disk volume information for \"%s\".\n", devname);
								printf("\tdev_res: %d / res: %u.\n", dev_res, res);
							}
						} else {
							//printf("\n\tLogical drive \"%c:\" not ready (empty drive?).\n", SingleDrive[0]);
							CloseHandle(drive);
							drive = INVALID_HANDLE_VALUE;
						}
					} else {
						printf("\n\tError opening logical drive \"%s\" (%d).\n", devname, GetLastError());
					}
				} else {
					if (res == DRIVE_UNKNOWN) printf("\n\tUnknown drive type for \"%c:\".\n", SingleDrive[0]);
				}
			}
			
			/* Get the next drive */
			SingleDrive += strlen(SingleDrive) + 1;
		}
		
		if (drive == INVALID_HANDLE_VALUE)
		{
			printf("\n\tError: Unable to identify the SD card that contains the EmuNAND.\n");
			goto boot_bin;
		}
	} else {
		printf("\n\tError parsing logical drive strings.\n");
		goto boot_bin;
	}
	
	printf("\n\tFound %.11s SD card!\n", (char*)(&(MAGIC_STR[mbr_str - 1][0])));
	printf("\tLogical drive: %s.\n", SingleDrive);
	printf("\tPhysical drive number: %d.\n", drivenum);
	
	/* Get disk geometry */
	int64_t drive_sz = get_drive_size(drive);
	if (drive_sz == -1)
	{
		printf("\n\tError getting disk geometry.\n");
		goto boot_bin;
	} else {
		printf("\tDrive capacity: %I64d bytes.\n", drive_sz);
		if ((!is_new && ((nandnum == 1 && ((drive_sz / 1000000) <= 1024)) || (nandnum > 1 && ((drive_sz / 1000000) <= 2048)))) || (is_new && ((nandnum == 1 && ((drive_sz / 1000000) <= 2048)) || (nandnum > 1 && ((drive_sz / 1000000) <= 4096)))))
		{
			printf("\n\tError: Your SD card must have a capacity of at least %c GB.\n", (nandnum == 1 ? (!is_new ? '2' : '4') : (!is_new ? '4' : '8')));
			goto boot_bin;
		}
	}
	
	/* Check if the SD card is write-protected */
	if (input_mode)
	{
		uint32_t sys_flags = 0;
		dev_res = GetVolumeInformation(SingleDrive, NULL, 0, NULL, NULL, (PDWORD)&sys_flags, NULL, 0);
		if (dev_res != 0)
		{
			if (sys_flags & FILE_READ_ONLY_VOLUME)
			{
				printf("\n\tError: The SD card is write-protected.\n");
				printf("\tMake sure the lock slider is not in the \"locked\" position.\n");
				goto boot_bin;
			}
		} else {
			printf("\n\tError: couldn't get the file system flags.\n");
			printf("\tNot really a critical problem. Process won't be halted.\n");
		}
	}
	
	/* Get drive layout */
	dev_res = DeviceIoControl(drive, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, DriveLayout, DriveLayoutLen, (PDWORD)&res, NULL);
	if (dev_res)
	{
		if (DriveLayout->PartitionEntry[0].Mbr.PartitionType == PARTITION_FAT32_LBA || DriveLayout->PartitionEntry[0].Mbr.PartitionType == PARTITION_FAT32 || DriveLayout->PartitionEntry[0].Mbr.PartitionType == PARTITION_FAT16)
		{
			/* Only use FAT16 if the SD card capacity is <= 4 GB or if the partition was already FAT16 */
			bool is_fat16 = (DriveLayout->PartitionEntry[0].Mbr.PartitionType == PARTITION_FAT16 || ((drive_sz / 1000000) <= 4096));
			
			if (nandnum > 1)
			{
				if (DriveLayout->PartitionEntry[0].StartingOffset.QuadPart < fatsector)
				{
					if (input_mode)
					{
						/* Move filesystem to the right */
						printf("\n\tFAT partition detected below the required offset (at 0x%09llX).\n", DriveLayout->PartitionEntry[0].StartingOffset.QuadPart);
						printf("\tBeginning drive layout modifying procedure... ");
						
						memset(DriveLayout, 0, DriveLayoutLen);
						
						DriveLayout->PartitionStyle = PARTITION_STYLE_MBR;
						DriveLayout->PartitionCount = 4; // Minimum required by MBR
						DriveLayout->Mbr.Signature = FAT32_SIGNATURE;
						
						DriveLayout->PartitionEntry[0].PartitionStyle = PARTITION_STYLE_MBR;
						DriveLayout->PartitionEntry[0].StartingOffset.QuadPart = fatsector;
						DriveLayout->PartitionEntry[0].PartitionLength.QuadPart = (drive_sz - fatsector);
						DriveLayout->PartitionEntry[0].PartitionNumber = 1;
						
						DriveLayout->PartitionEntry[0].Mbr.PartitionType = (is_fat16 ? PARTITION_FAT16 : PARTITION_FAT32_LBA);
						DriveLayout->PartitionEntry[0].Mbr.BootIndicator = FALSE;
						DriveLayout->PartitionEntry[0].Mbr.RecognizedPartition = 1;
						DriveLayout->PartitionEntry[0].Mbr.HiddenSectors = 0;
						
						for (i = 0; i < 4; i++) DriveLayout->PartitionEntry[i].RewritePartition = TRUE;
						
						dev_res = DeviceIoControl(drive, IOCTL_DISK_SET_DRIVE_LAYOUT_EX, DriveLayout, DriveLayoutLen, NULL, 0, (PDWORD)&res, NULL);
						if (dev_res)
						{
							printf("OK!\n\tFAT partition successfully moved to offset 0x%09llX!\n", fatsector);
							
							char format_cmd[64] = {0};
							snprintf(format_cmd, MAX_CHARACTERS(format_cmd), "FORMAT %c: /Y /FS:%s /V:%.11s /Q /X", SingleDrive[0], (is_fat16 ? "FAT" : "FAT32"), (char*)(&(MAGIC_STR[mbr_str - 1][0])));
							
							CloseHandle(drive);
							
							/* Format the new partition */
							print_divider_bar();
							dev_res = system(format_cmd);
							print_divider_bar();
							
							if (dev_res == 0)
							{
								/* Reopen the handle to the physical drive */
								drive = CreateFile(devname, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
								if (drive != INVALID_HANDLE_VALUE)
								{
									printf("\tSuccessfully formatted the new partition.\n");
									
									/* Set file pointer to (nandsect - SECTOR_SIZE) and write the dummy header */
									if (!write_dummy_data(drive, nandsect - SECTOR_SIZE, true)) goto boot_bin;
								} else {
									printf("\tError reopening the handle to the physical drive.\n");
									goto boot_bin;
								}
							} else {
								printf("\tError formatting the new partition! (%d).\n", dev_res);
								goto boot_bin;
							}
						} else {
							printf("\n\tFatal error: Couldn't modify the drive layout.\n");
							goto boot_bin;
						}
					} else {
						printf("\n\tError: FAT partition offset (0x%09llX) collides with the\n", DriveLayout->PartitionEntry[0].StartingOffset.QuadPart);
						printf("\t**%s** EmuNAND offset (0x%09llX). The %s %c GiB\n", NAND_NUM_STR(nandnum), nandsect, NAND_NUM_STR(nandnum), (!is_new ? '1' : '2'));
						printf("\tsegment after 0x%09llX hasn't been created!\n", nandsect - SECTOR_SIZE);
						goto boot_bin;
					}
				} else {
					printf("\n\tFAT partition already positioned beyond offset 0x%09llX.\n", fatsector - 1);
					if (input_mode) printf("\tSkipping drive layout modification procedure.\n");
				}
			} else {
				if (DriveLayout->PartitionEntry[0].StartingOffset.QuadPart < fatsector)
				{
					printf("\n\tError: FAT partition offset (0x%09llX) collides with the\n", DriveLayout->PartitionEntry[0].StartingOffset.QuadPart);
					printf("\t**%s** EmuNAND offset (0x200). This is probably some\n\n", NAND_NUM_STR(nandnum));
					printf("\tkind of corruption. Format the EmuNAND again on your\n");
					printf("\tNintendo 3DS console to fix this.\n");
					goto boot_bin;
				} else {
					printf("\n\tFAT partition already positioned beyond offset 0x%09llX.\n", fatsector - 1);
				}
			}
		} else {
			printf("\n\tInvalid partition type: %02X.\n", DriveLayout->PartitionEntry[0].Mbr.PartitionType);
			printf("\tYou should report this to me ASAP.\n");
			goto boot_bin;
		}
	} else {
		printf("\n\tError getting drive layout (%d).\n", GetLastError());
		goto boot_bin;
	}
	
	int64_t offset = 0;
	uint32_t nandsize = 0, magic_word = 0;
	
	/* Open 3DS NAND dump */
	nandfile = fopen(argv[4], (input_mode ? "rb" : "wb"));
	if (!nandfile)
	{
		printf("\n\tError opening \"%s\" for %s.\n", argv[4], (input_mode ? "reading" : "writing"));
		goto boot_bin;
	}
	
	if (input_mode)
	{
		/* Store NAND dump size */
		fseek(nandfile, 0, SEEK_END);
		nandsize = ftell(nandfile);
		rewind(nandfile);
		
		if (nandsize > 0 && ((!is_new && (nandsize == TOSHIBA_NAND || nandsize == TOSHIBA_REDNAND || nandsize == SAMSUNG_NAND || nandsize == SAMSUNG_REDNAND)) || (is_new && (nandsize == N3DS_SAMSUNG_NAND || N3DS_UNKNOWN_NAND))))
		{
			bool is_rednand = (nandsize == TOSHIBA_REDNAND || nandsize == SAMSUNG_REDNAND);
			if (is_rednand && !cfw) cfw = true; // Override configuration
			
			printf("\n\t%s 3DS %s NAND dump detected!\n", (!is_new ? "Old" : "New"), NAND_TYPE_STR(nandsize));
			printf("\tFilesize: %u bytes.\n", nandsize);
			
			/* Check if the supplied NAND dump does contain an NCSD header */
			fseek(nandfile, (is_rednand ? (SECTOR_SIZE + 0x100) : 0x100), SEEK_SET);
			fread(&magic_word, 4, 1, nandfile);
			rewind(nandfile);
			
			if (magic_word == bswap_32(NCSD_MAGIC))
			{
				printf("\n\tValid NCSD header detected at offset 0x%08x.\n", (is_rednand ? SECTOR_SIZE : 0));
				
				if (is_rednand)
				{
					/* Skip the dummy header (if it's already present) */
					fseek(nandfile, SECTOR_SIZE, SEEK_SET);
					nandsize -= SECTOR_SIZE;
				}
			} else {
				printf("\n\tInvalid 3DS NAND dump.\n");
				printf("\tNCSD header is missing.\n");
				goto boot_bin;
			}
		} else {
			printf("\n\tInvalid 3DS NAND dump.\n");
			printf("\tFilesize (%u bytes) is invalid.\n", nandsize);
			goto boot_bin;
		}
	} else {
		/* Check if the NAND dump stored in the SD card is valid */
		/* Depending on the type of the NAND stored in the SD card, the data has to be read in different ways */
		/* This is because the order in which the data is written varies between each type of NAND */
		
		/* EmuNAND: the first 0x200 bytes (NCSD header) are stored **after** the NAND dump. The NAND dump starts to be written */
		/*			from offset 0x200 to the SD card */
		
		/* RedNAND: used by the Custom Firmware (CFW). It is written to the SD card 'as is', beginning with the NCSD header and */
		/*			following with the rest of the NAND data */
		
		/* Usually, a dummy footer follows afterwards. This applies for both types of NANDs, and serves to indicate the appropiate */
		/* NAND flash capacity of the 3DS console */
		
		/* The CFW is so far the only loading method that supports using a custom boot sector (different to 1) */
		/* This is expected to change in the near future */
		
		for (i = 0; i <= 2; i++)
		{
			if (is_new && i == 2) break;
			
			/* Old 3DS: Check if this is a RedNAND (i = 0), Toshiba EmuNAND (i = 1) or Samsung EmuNAND (i = 2), in that order */
			/* New 3DS: Check if this is a N3DS Samsung EmuNAND (i = 0) or a N3DS **Unknown** EmuNAND (i = 1), in that order */
			
			switch (i)
			{
				case 0:
					if (!is_new)
					{
						/* Old 3DS RedNAND */
						offset = nandsect;
					} else {
						/* New 3DS Samsung EmuNAND */
						offset = (nandsect + N3DS_SAMSUNG_NAND - SECTOR_SIZE);
					}
					break;
				case 1:
					if (!is_new)
					{
						/* Old 3DS Toshiba EmuNAND */
						offset = (nandsect + TOSHIBA_NAND - SECTOR_SIZE);
					} else {
						/* New 3DS **Unknown** EmuNAND */
						offset = (nandsect + N3DS_UNKNOWN_NAND - SECTOR_SIZE);
					}
					break;
				case 2:
					/* Old 3DS Samsung EmuNAND */
					offset = (nandsect + SAMSUNG_NAND - SECTOR_SIZE);
					break;
				default:
					break;
			}
			
			cur_ptr = set_file_pointer(drive, offset, FILE_BEGIN);
			if (cur_ptr != -1)
			{
				/* Remember that read/write operations must be aligned to 512 bytes */
				dev_res = ReadFile(drive, buf, SECTOR_SIZE, (PDWORD)&res, NULL);
				if (dev_res && res == SECTOR_SIZE)
				{
					memcpy(&magic_word, &(buf[0x100]), 4);
					if (magic_word == bswap_32(NCSD_MAGIC))
					{
						printf("\n\tValid NCSD header detected at offset 0x%09llX (%s).\n", offset, ((i == 0 && !is_new) ? "RedNAND" : "EmuNAND"));
						
						/* No need to calculate what we already know */
						switch (i)
						{
							case 0:
								if (is_new) nandsize = N3DS_SAMSUNG_NAND;
								break;
							case 1:
								nandsize = (!is_new ? TOSHIBA_NAND : N3DS_UNKNOWN_NAND);
								break;
							case 2:
								nandsize = SAMSUNG_NAND;
								break;
							default:
								break;
						}
						
						/* RedNAND size calculation procedure */
						if (nandsize == 0)
						{
							/* Override configuration */
							cfw = true;
							
							/* Calculate NAND dump size (based in the dummy footer position) */
							/* This steps will only be performed on a RedNAND */
							int j = 0;
							uint16_t data = 0;
							uint8_t dummy[SECTOR_SIZE] = {0};
							
							while (j < 2)
							{
								/* Check if this is a Toshiba RedNAND (j = 0) or Samsung RedNAND (j = 1), in that order */
								offset = (j == 0 ? (nandsect + TOSHIBA_NAND) : (nandsect + SAMSUNG_NAND));
								cur_ptr = set_file_pointer(drive, offset, FILE_BEGIN);
								if (cur_ptr == -1) break;
								
								/* Can't lose that copy of the NCSD header stored in buf. We'll be needing it at a later time */
								dev_res = ReadFile(drive, dummy, SECTOR_SIZE, (PDWORD)&res, NULL);
								if (!dev_res || res != SECTOR_SIZE) break;
								
								/* Check if this block contains the dummy footer */
								int k;
								for (k = 0; k < SECTOR_SIZE; k += 2)
								{
									memcpy(&data, &(dummy[k]), 2);
									if (data != bswap_16(DUMMY_DATA)) break;
								}
								
								if (data == bswap_16(DUMMY_DATA))
								{
									/* Found it */
									nandsize = (j == 0 ? TOSHIBA_NAND : SAMSUNG_NAND);
									break;
								}
								
								j++;
							}
							
							if (cur_ptr == -1 || !dev_res || res != SECTOR_SIZE || nandsize > 0) break;
							
							printf("\n\tDummy footer not available in the SD card.\n");
							printf("\tRedNAND size will be calculated based in the NCSD header info.\n");
							printf("\tIt may not match your actual NAND flash capacity.\n");
							
							/* Calculate NAND dump size (based in the NCSD header info) */
							/* May not match actual NAND flash capacity */
							for (j = 0x124; j < 0x160; j += 8)
							{
								uint32_t partition_len = 0;
								memcpy(&partition_len, &(buf[j]), 4);
								nandsize += (partition_len * MEDIA_UNIT_SIZE);
							}
						}
						
						break;
					}
				} else {
					break;
				}
			} else {
				break;
			}
		}
		
		if (cur_ptr == -1)
		{
			printf("\n\tError seeking to offset 0x%09llX in physical drive.\n", offset);
			goto boot_bin;
		} else {
			if (!dev_res || res != SECTOR_SIZE)
			{
				printf("\n\tError reading %d bytes block from offset 0x%09llX.\n", SECTOR_SIZE, offset);
				goto boot_bin;
			} else {
				if (magic_word != bswap_32(NCSD_MAGIC))
				{
					printf("\n\tInvalid 3DS NAND dump.\n");
					printf("\tNCSD header is missing.\n");
					goto boot_bin;
				} else {
					if (nandsize == TOSHIBA_NAND || nandsize == SAMSUNG_NAND || nandsize == N3DS_SAMSUNG_NAND || nandsize == N3DS_UNKNOWN_NAND)
					{
						printf("\n\t%s 3DS %s NAND dump detected!\n", (!is_new ? "Old" : "New"), NAND_TYPE_STR(nandsize));
						printf("\tFilesize: %u bytes.\n", nandsize);
					} else {
						printf("\n\tInvalid 3DS NAND dump.\n");
						printf("\tFilesize (%u bytes) is invalid.\n", nandsize);
						goto boot_bin;
					}
				}
			}
		}
	}
	
	uint32_t cnt;
	uint8_t *nand_buf = malloc(NAND_BUF_SIZE);
	
	printf("\n\t%s %s %sNAND %s the SD card, please wait...\n\n", (input_mode ? "Writing" : "Reading"), NAND_NUM_STR(nandnum), (cfw ? "Red" : "Emu"), (input_mode ? "to" : "from"));
	
	/* The real magic begins here */
	for (cnt = 0; cnt < nandsize; cnt += NAND_BUF_SIZE)
	{
		/* Set file pointer before doing any read/write operation */
		/* Remember to appropiately set the file pointer to the end of the NAND dump when dealing with the NCSD header (EmuNAND only) */
		offset = (cfw ? (nandsect + cnt) : (cnt > 0 ? (nandsect - SECTOR_SIZE + cnt) : (input_mode ? (nandsect - SECTOR_SIZE + nandsize) : (nandsect - SECTOR_SIZE))));
		cur_ptr = set_file_pointer(drive, offset, FILE_BEGIN);
		if (cur_ptr == -1) break;
		
		if (input_mode)
		{
			/* Fill buffer (file) */
			fread(nand_buf, NAND_BUF_SIZE, 1, nandfile);
			
			if (!cfw && cnt == 0)
			{
				/* Write the NCSD header contained in nand_buf */
				dev_res = WriteFile(drive, nand_buf, SECTOR_SIZE, (PDWORD)&res, NULL);
				if (!dev_res || res != SECTOR_SIZE) break;
				
				/* Go back to sector #1 */
				offset = nandsect;
				cur_ptr = set_file_pointer(drive, offset, FILE_BEGIN);
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
				if (!write_dummy_data(drive, (cfw ? (offset + NAND_BUF_SIZE) : (offset + NAND_BUF_SIZE + SECTOR_SIZE)), false))
				{
					printf("\n\tError writing dummy data.\n");
					break;
				}
			}
		} else {
			/* Fill buffer (SD) */
			dev_res = ReadFile(drive, nand_buf, NAND_BUF_SIZE, (PDWORD)&res, NULL);
			if (!dev_res || res != NAND_BUF_SIZE) break;
			
			/* Replace the first 512-bytes block in nand_buf with the NCSD header (still contained in buf) */
			if (!cfw && cnt == 0) memcpy(nand_buf, buf, SECTOR_SIZE);
			
			/* Write buffer (file) */
			fwrite(nand_buf, 1, NAND_BUF_SIZE, nandfile);
		}
		
		print_progress(cnt + NAND_BUF_SIZE, nandsize);
	}
	
	free(nand_buf);
	printf("\n");
	
	if (cur_ptr == -1)
	{
		printf("\n\tError seeking to offset 0x%09llX in physical drive.\n", offset);
		goto boot_bin;
	} else
	if (!dev_res || (cnt == 0 && !cfw && res != (NAND_BUF_SIZE - SECTOR_SIZE)) || (cnt > 0 && res != NAND_BUF_SIZE))
	{
		printf("\n\tError %s block #%u %s offset 0x%09llX.\n", (input_mode ? "writing" : "reading"), cnt / NAND_BUF_SIZE, (input_mode ? "to" : "from"), offset);
		goto boot_bin;
	}
	
boot_bin:
	/* Modify the boot.bin file from the Palantine CFW. This is entirely optional */
	if (argc == 6 && !is_new)
	{
		FILE *boot_bin = fopen(argv[5], "rb+");
		if (!boot_bin)
		{
			printf("\n\tError opening \"%s\" for reading and writing.\n", argv[5]);
		} else {
			/* Store boot.bin size */
			fseek(boot_bin, 0, SEEK_END);
			uint32_t bootsize = ftell(boot_bin);
			rewind(boot_bin);
			
			printf("\n\tboot.bin size: %u bytes ", bootsize);
			
			if (bootsize > 0 && bootsize == BOOT_BIN_SIZE)
			{
				printf("(good).\n");
				
				/* The boot.bin file stores the SD card sector number from which the RedNAND will be booted at offset 0x14 */
				/* Please note that it is a Little Endian value */
				
				/* Original value:	0x00000001 (sector #1) */
				/* Second RedNAND:	0x00200001 (sector #2097153) */
				/* Third RedNAND:	0x00400001 (sector #4194305) */
				
				fseek(boot_bin, BOOT_BIN_SECTOR, SEEK_SET);
				
				uint32_t bootsect;
				fread(&bootsect, 4, 1, boot_bin);
				fseek(boot_bin, -4, SEEK_CUR);
				
				printf("\tboot.bin boot sector: %u (offset 0x%08x).\n", bootsect, bootsect * SECTOR_SIZE);
				
				uint32_t newsect = (nandsect / SECTOR_SIZE);
				printf("\tNew boot sector: %u (offset 0x%08x).\n", newsect, newsect * SECTOR_SIZE);
				
				if (bootsect == newsect)
				{
					printf("\n\tThis boot.bin file was already modified.\n");
				} else {
					fwrite(&newsect, 1, 4, boot_bin);
					printf("\n\tboot.bin file successfully modified.\n");
				}
			} else {
				printf("(bad).\n\tMake sure you're using the appropiate Palantine CFW files.\n");
			}
			
			fclose(boot_bin);
		}
	}
	
	if (DriveLayout) free(DriveLayout);
	if (nandfile) fclose(nandfile);
	if (drive != INVALID_HANDLE_VALUE) CloseHandle(drive);
	
	return 0;
}

/* From the "How To Determine Whether a Thread Is Running in User Context of Local Administrator Account" article */
/* URL: https://support.microsoft.com/en-us/kb/118626 */
bool IsUserAdmin(void)
{
	PACL pACL = NULL;
	PRIVILEGE_SET ps;
	PSID psidAdmin = NULL;
	GENERIC_MAPPING GenericMapping;
	PSECURITY_DESCRIPTOR psdAdmin = NULL;
	HANDLE hAccessToken = NULL, hImpersonationToken = NULL;
	SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
	DWORD dwStatus, dwAccessMask, dwAccessDesired, dwACLSize, dwStructureSize = sizeof(PRIVILEGE_SET);
	
	const DWORD ACCESS_READ  = 1;
	const DWORD ACCESS_WRITE = 2;
	
	BOOL success = OpenThreadToken(GetCurrentThread(), TOKEN_DUPLICATE|TOKEN_QUERY, TRUE, &hAccessToken);
	if (!success)
	{
		if (GetLastError() == ERROR_NO_TOKEN) success = OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE|TOKEN_QUERY, &hAccessToken);
	}
	
	if (success)
	{
		success = DuplicateToken(hAccessToken, SecurityImpersonation, &hImpersonationToken);
		if (success)
		{
			success = AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &psidAdmin);
			if (success)
			{
				psdAdmin = LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
				if (psdAdmin != NULL)
				{
					success = InitializeSecurityDescriptor(psdAdmin, SECURITY_DESCRIPTOR_REVISION);
					if (success)
					{
						dwACLSize = sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) + GetLengthSid(psidAdmin) - sizeof(DWORD);
						pACL = (PACL)LocalAlloc(LPTR, dwACLSize);
						if (pACL != NULL)
						{
							success = InitializeAcl(pACL, dwACLSize, ACL_REVISION2);
							if (success)
							{
								dwAccessMask = ACCESS_READ | ACCESS_WRITE;
								success = AddAccessAllowedAce(pACL, ACL_REVISION2, dwAccessMask, psidAdmin);
								if (success)
								{
									success = SetSecurityDescriptorDacl(psdAdmin, TRUE, pACL, FALSE);
									if (success)
									{
										SetSecurityDescriptorGroup(psdAdmin, psidAdmin, FALSE);
										SetSecurityDescriptorOwner(psdAdmin, psidAdmin, FALSE);
										
										success = IsValidSecurityDescriptor(psdAdmin);
										if (success)
										{
											dwAccessDesired = ACCESS_READ;
											
											GenericMapping.GenericRead = ACCESS_READ;
											GenericMapping.GenericWrite = ACCESS_WRITE;
											GenericMapping.GenericExecute = 0;
											GenericMapping.GenericAll = ACCESS_READ | ACCESS_WRITE;
											
											AccessCheck(psdAdmin, hImpersonationToken, dwAccessDesired, &GenericMapping, &ps, &dwStructureSize, &dwStatus, &success);
										}
									}
								}
							}
						} else {
							success = FALSE;
						}
					}
				} else {
					success = FALSE;
				}
			}
		}
	}
	
	if (pACL) LocalFree(pACL);
	if (psdAdmin) LocalFree(psdAdmin);
	if (psidAdmin) FreeSid(psidAdmin);
	if (hImpersonationToken) CloseHandle(hImpersonationToken);
	if (hAccessToken) CloseHandle(hAccessToken);
	
	return success;
}

void print_info()
{
	printf("\n\tUsage:\n");
	printf("\t3ds-multinand [-old|-new] [-1|-2|-3] [-i|-o|-cfw] [nand.bin] [boot.bin]\n\n");
	
	printf("\tThis program will automatically detect the SD card that\n");
	printf("\tcontains an already formatted EmuNAND. Make sure to run\n");
	printf("\tit with administrative privileges!\n\n\n");
	
	
	printf("\tNintendo 3DS model parameters:\n\n");
	
	printf("\t * \"-old\" option: Use SD card offsets compatible with the\n");
	printf("\t   Old Nintendo 3DS.\n\n");
	
	printf("\t * \"-new\" option: Use SD card offsets compatible with the\n");
	printf("\t   New Nintendo 3DS.\n\n\n");
	
	
	printf("\tEmuNAND selection parameters:\n\n");
	
	printf("\t * \"-1\" option: Write/read the **first** EmuNAND.\n\n");
	
	printf("\t * \"-2\" option: Write/read the **second** EmuNAND.\n\n");
	
	printf("\t * \"-3\" option: Write/read the **third** EmuNAND.\n\n");
	
	printf("\t * \"-4\" option: Write/read the **fourth** EmuNAND.\n\n\n");
	
	
	printf("\tOperation parameters:\n\n");
	
	printf("\t * \"-i\" (input) option: Write the specified NAND dump to the SD\n");
	printf("\t   card as the first, second, third or fourth EmuNAND (depending on\n");
	printf("\t   the value of the previous parameter).\n\n");
	
	printf("\t * \"-cfw\" (input) option: Write the specified NAND dump to the SD\n");
	printf("\t   card as the first, second, third or fourth **RedNAND** (depending\n");
	printf("\t   on the value of the previous parameter). This makes the written\n");
	printf("\t   NAND compatible with the Palantine CFW.\n\n");
	
	printf("\t * \"-o\" (output) option: Dump the first, second, third or fourth\n");
	printf("\t   EmuNAND (depending on the value of the previous parameter) to the\n");
	printf("\t   specified file.\n\n\n");
	
	
	printf("\tNotes:\n\n");
	
	printf("\t * If a writing operation is performed to the **second**, **third** or\n");
	printf("\t   **fourth** EmuNAND offsets in the SD card, and the available FAT\n");
	printf("\t   partition collides with it, it'll get moved to the right and it'll\n");
	printf("\t   be quick-formatted. Make sure to backup your data before using the\n");
	printf("\t   \"-2\", \"-3\" or \"-4\" parameters with \"-i\" or \"-cfw\"!\n\n");
	
	printf("\t * It isn't necessary to add a 512-bytes dummy header to the NAND dump\n");
	printf("\t   (e.g., running \"drag_emunand_here.bat\" on it) before using this\n");
	printf("\t   program, even though it is compatible with such dumps. If you want\n");
	printf("\t   to write the input NAND dump as a RedNAND, just use the \"-cfw\"\n");
	printf("\t   parameter instead of \"-i\".\n\n");
	
	printf("\t * Optionally, you can provide a copy of the boot.bin file from the\n");
	printf("\t   Palantine CFW. The program will modify the boot sector stored at\n");
	printf("\t   offset 0x%02x to match the one used by the RedNAND number you specify.\n\n", BOOT_BIN_SECTOR);
	
	printf("\t * If you use the \"-new\" parameter, you won't be able to write an input\n");
	printf("\t   NAND dump as a RedNAND, because the Palantine CFW is not compatible\n");
	printf("\t   with the New Nintendo 3DS. Please bear that in mind.\n\n\n");
	
	printf("\tThanks to nop90, for reverse-engineering the boot.bin file from the CFW.\n");
}

int64_t set_file_pointer(HANDLE h, int64_t new_ptr, uint32_t method)
{
	int32_t hi_ptr = PTR_HIGH(new_ptr);
	int32_t lo_ptr = PTR_LOW(new_ptr);

	lo_ptr = SetFilePointer(h, lo_ptr, (PLONG)&hi_ptr, method);
	if (lo_ptr != -1 && hi_ptr != -1)
	{
		//printf("\n\t\t\tcur_ptr: 0x%08x%08x.", hi_ptr, lo_ptr);
		return PTR_FULL(hi_ptr, lo_ptr);
	}
	
	return -1;
}

bool write_dummy_data(HANDLE SDcard, int64_t offset, bool verbose)
{
	int64_t ptr = set_file_pointer(SDcard, offset, FILE_BEGIN);
	if (ptr != -1)
	{
		uint32_t bytes = 0;
		uint8_t dummy_buf[SECTOR_SIZE] = {0};
		
		/* Fill buffer with dummy data */
		int i;
		for (i = 0; i < SECTOR_SIZE; i += 2)
		{
			dummy_buf[i] = ((DUMMY_DATA >> 8) & 0xff);
			dummy_buf[i+1] = (DUMMY_DATA & 0xff);
		}
		
		/* Write dummy data */
		int res = WriteFile(SDcard, dummy_buf, SECTOR_SIZE, (PDWORD)&bytes, NULL);
		if (res && bytes == SECTOR_SIZE)
		{
			if (verbose) printf("\tWrote %d bytes long \"0x%04X\" dummy data at offset 0x%09llX.\n", SECTOR_SIZE, DUMMY_DATA, offset);
			return true;
		} else {
			if (verbose) printf("\tError writing dummy data.\n");
		}
	} else {
		if (verbose) printf("\tError seeking to offset 0x%09llX in physical drive.\n", offset);
	}
	
	return false;
}

int64_t get_drive_size(HANDLE h)
{
	uint32_t status, returned;
	DISK_GEOMETRY_EX DiskGeometry;
	
    status = DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &DiskGeometry, sizeof(DISK_GEOMETRY_EX), (PDWORD)&returned, NULL);
    if (!status)
	{
		printf("\n\tDeviceIoControl failed (%d).\n", GetLastError());
		return -1;
	}
	
	if (DiskGeometry.DiskSize.QuadPart > 0) return DiskGeometry.DiskSize.QuadPart;
	
	return -1;
}

void print_progress(uint32_t read_amount, uint32_t fullsize)
{
	static uint32_t last_percent = 0;
	uint32_t j, percent = (uint32_t)((((double)read_amount)/fullsize)*100), bar_size = (uint32_t)((((double)percent)/100)*BAR_LEN);
	
	if (percent > last_percent)
	{
		printf("\t[");
		for (j = 0; j < BAR_LEN; j++)
		{
			if (j < bar_size)
			{
				printf("=");
			} else
			if (j == bar_size)
			{
				printf(">");
			} else {
				printf(" ");
			}
		}
		
		printf("] %u%%\r", percent);
		fflush(stdout);
		
		last_percent = percent;
	}
}

void print_divider_bar()
{
	int i;
	
	printf("\t");
	for (i = 0; i < 64; i++) printf("%c", '_');
	printf("\n\n");
}
