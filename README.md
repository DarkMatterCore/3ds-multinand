# 3ds-multinand
3DS Multi EmuNAND Creator

This program is designed to inject/extract NAND dumps to/from the storage area reserved in the SD card by tools like Gateway's Launcher.dat and EmuNAND9. It is fully written in C and can be compiled using MinGW or TDM-GCC, without the need of additional libraries and/or runtime components.

This code is licensed under GPLv3.

Features
--------------

* Automatic detection of SD cards that contain one or more NAND(s).
* Can inject/extract up to four (4) different NANDs to/from any given SD card (as long as the storage capacity allows it).
* Compatible with Old 3DS/2DS and New 3DS/2DS NAND dumps.
* Supports both EmuNAND and RedNAND (+1 sector offset) formats.
* Compatible with all the existent NAND layouts: 'Legacy' (used by Gateway's Launcher.dat), 'Default' (4MB-rounded NAND size) and 'Minimum' (4MB-rounded minimum NAND size possible).
* Capable of performing the "Format EmuNAND" action, which allows to partition, format and inject a NAND dump to a new SD card without using a Nintendo 3DS console.
* Ability to remove an existent NAND from the SD card by repartitioning and reformating it, gaining more space for the FAT partition in process. If an NAND that precedes one or more additional NANDs is removed, these will also get lost. The removal of NAND #1 is equivalent to formatting the SD card without an EmuNAND, e.g. you will gain back all the space consumed by the NAND(s).
* Ability to set a custom name to a NAND, which can be displayed by CakesFW in its Multi NAND selection menu.

Thanks to
--------------

* Pete Batard, for developing Rufus. This program uses code from Rufus in its FAT32 format procedure.
* All my friends, who helped and motivated me to keep moving forward.
* The folks at GBAtemp, for testing every new release.

Changelog
--------------

**v1.71:**
* Updated program manifest to target at more Windows versions.
* Added a proper Windows version check. The program won't run under a Windows version older than XP SP2.
* Fixed a bug where the DRIVE_INFO struct pointer could be freed twice, leading to a crash when trying to refresh the drives lists.
* Simplified the code for checking if the program is running under admin mode.

**v1.7:**
* Completely revamped the EmuNAND parsing code.
* Added compatibility with multiple NAND layouts in the same SD card. The program is now able to detect, read and write EmuNANDs that don't share the same layout as the first one.
* Every NAND inject operation now defaults to the minimum size possible, including the format procedure. This should fix some compatibility problems with Luma3DS.
* If a new NAND is injected and the size of the previous NAND was greater than the minimum size possible, and if it wasn't stored as a RedNAND, its NCSD header will be wiped from the SD card to avoid problems.

**v1.61:**
* Fixed a NAND offset miscalculation while parsing EmuNANDs.
* Fixed a regression bug where the NCSD header was always written/read to/from offset "nandsize + 512" instead of the appropiate sector (EmuNAND only).
* Fixed a bug where the calculated NAND size would be zero if a new EmuNAND with a size greater than the current layout was being injected.
* Improved FAT32 format procedure reliability (based on code from Rufus).
* Fixed a crash that occurred when attempting to format a new FAT32 partition (debug build only).
* The program is now licensed under GPLv3.

**v1.6:**
* Added support for 1.82 GiB N3DS NAND dumps.
* Added proper detection of 2DS NAND dumps.
* The "Format EmuNAND" procedure now defaults to the 'minimum' setup size, using a RedNAND.
* The program now forces the "Remove NAND" button to be enabled if a previous attempt to remove the first EmuNAND failed.
* The MBR is now manually initialized with zeros before creating it using WinAPI instructions (leftover data is kept in sector #0 otherwise).

**v1.5:**
* Added a checkbox to determine if fixed drives have to be added to the drop-down lists while the program is parsing the currently available storage drives (suggested by piratesephiroth).
* The program now retrieves the extended partition information when parsing the currently available drives in order to get a proper starting offset, instead of manually calculating it based on the MBR information (which may not exist at all).
* Added a "Remove EmuNAND" button, which removes the selected EmuNAND from the SD card. Please bear in mind that this procedure modifies the partition table from the MBR, so backup your data before using it. If an EmuNAND that precedes one or more additional EmuNANDs is removed, these will also get lost. The removal of EmuNAND #1 is equivalent to formatting the SD card without an EmuNAND, e.g. you will gain back all the space consumed by the EmuNAND(s).
* Fixed a bug where the program wouldn't format the FAT32 partition of the selected drive after a previous run of the format procedure failed *but* the partition table was successfully modified.
* The program will now add a valid MBR to sector #0 if it isn't found during the format procedure.

**v1.4:**
* Autodetection of Old 3DS/New 3DS EmuNANDs for both input/output procedures, which removes the need of manually selecting a 3DS model.
* Added support for additional EmuNAND9 offsets. Every EmuNAND/RedNAND is now created using the 'default' setup size (except if you're creating additional EmuNANDs, in which case the same layout from EmuNAND #1 will be applied).
* The program now shows the capacity of each drive displayed in the drop-down lists. Furthermore, only drives with a capacity greater than 1 GiB will be displayed in the drop-down lists.
* The program now checks if a valid EmuNAND is available at the selected slot before writing the name to the SD card.
* Fixed a bug that made the "Refresh" button also write the EmuNAND name instead of just reading it.
* The FAT32 partition label is now properly written during the format procedure.
* Removed the boot.bin modification feature.
* Fixed some more threading bugs.

**v1.3:**
* It is now possible to set a custom name to an EmuNAND, which can be displayed by CakesFW in its Multi EmuNAND selection menu.
* The program is now able to format the FAT32 partition without calling the format.exe tool from Windows, using code from fat32format by Ridgecrop.
* Added compatibility with exFAT SD cards. Please have in mind that these cards *will* get formatted to FAT32, if they already aren't.
* Fixed some threading bugs.

**v1.2:**
* Fixed compatibility with EmuNANDs generated with the EmuNAND9 Tool (different FAT32 starting offset).

**v1.1:**
* A new EmuNAND SD Card drop-down list has been added, which is automatically populated with drives that have a compatible flashcard string in their MBR. This allows the user to select a specific drive for the input/output operations if more than one SD card with a valid flashcard string is available, instead of letting the program select the first one it finds.
* Physical disks will now only be added once to the drive lists if they have more than one logical partition.
* Changed the "Format New EmuNAND" and "Update" tags to "Format EmuNAND" and "Refresh", respectively. They were a little confusing.
* Fixed compatibility with FAT16 partitions (types 0x04 and 0x06).
* Fixed compatibility with the GPT partition style when formatting an EmuNAND. Please note the partition style *will* get changed to MBR after the EmuNAND format procedure.

**v1.0:**
* Added the "Format EmuNAND" feature, which allows the users to format an EmuNAND in a new SD card without using a Nintendo 3DS console, as long as a proper NAND dump is given to the program.

**v0.9:**
* The program now shows a warning if the "Inject EmuNAND" button is clicked and if the input NAND dump was previously patched using the "drag_emunand_here" batch file.
* Reimplemented the boot.bin modification feature. Make sure you set the "NAND Number" option to the RedNAND you want to boot, and then click the "Modify boot.bin" button.
* Minor UI changes.

**v0.8:**
* Program ported to Win32 API, so now it finally has a proper GUI. It still is written in C and compiled with MinGW. Props to my good ol' friend Xiangua/Nana from a certain Spanish forum, for designing the program icon.
* Buffer size changed from 32 KiB to 64 KiB.

**v0.7:**
* Program named changed yet again, this time around to "3DS Multi EmuNAND Creator", or "3ds-multinand" for short. Props to Cyan from GBAtemp for the idea (yeah, I'm *that* bad with names).
* Added compatibility with a fourth EmuNAND, because why not? Just use the "-4" parameter.
* The program now checks if the target SD card is write-protected if an input mode is used.
* The program now checks if it is running with administrative privileges. I hope this will keep some people from asking why the program isn't working appropiately...
* Added compatibility with the "3DSCARDNAND" MBR string, since some other 3DS flashcards apparently use it.
* Added compatibility with New 3DS NAND dumps (with multi EmuNAND support). Program usage has been changed again, so please check out the help screen. To make the program use the New 3DS specific offsets, just change argument #1 to "-new". If you just want to keep using the Old 3DS offsets, use "-old" instead. Please bear in mind that the "-cfw" parameter won't do any good while using the "-new" option, since the Palantine CFW isn't compatible with the New 3DS to begin with.

**v0.6 (beta 3):**
* The program now checks if a logical drive is ready before closing the handle and trying to open its physical drive.

**v0.6 (beta 2):**
* Fixed compatibility with FAT16 partitions.

**v0.6 (beta):**
* Name changed from "3ds-dualnand" to "3ds-triplenand".
* The program is now able to write an EmuNAND to the second RedNAND offset in the SD card. Previously, it was hardcoded to only write a RedNAND to this offset, even when using the "-i" parameter.
* Now, the program is able to configure a third EmuNAND/RedNAND by moving the FAT32 partition further to the right, increasing the size of the unallocated space reserved for the NANDs.

**v0.5:**
* FAT32 format procedure no longer requires any user input.
* Fixed compatibility with RedNANDs (NAND dumps with a 512 bytes long dummy header).
* You can now use the "-cfw" parameter instead of "-i" to write the input NAND dump as a RedNAND (for use with the Palantine CFW).
* Corrected a previous behaviour where EmuNANDs were being written as RedNANDs for "GATEWAYNAND" SD cards (only if the "-1" parameter was used). The NCSD header is now written to the appropiate offset.
* The application now writes a dummy footer after the NAND dump in the SD card during any given input operation, which serves to indicate the right size of the output NAND dump when doing an extraction. This is because the size calculation based in the NCSD header info doesn't always match the right NAND flash capacity.

**v0.4:**
* Added compatibility with the first EmuNAND. To select an EmuNAND, use either the "-1" (first EmuNAND) or "-2" (second EmuNAND) option, along with the operation parameter (either "-i" or "-o").

**v0.3:**
* You can now specify an input ("-i") or output ("-o") parameter, to either write or extract the second EmuNAND.

**v0.2:**
* Added compatibility with SD cards that contain an EmuNAND created by the Gateway Launcher.dat.

**v0.1:**
* Initial release, only able to write a second EmuNAND to the SD card.
