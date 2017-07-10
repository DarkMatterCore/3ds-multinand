#include <windows.h>
#include <commctrl.h>
#include "resource.h"
#include "3ds-multinand.h"

#define WINDOW_WIDTH	320
#define WINDOW_HEIGHT	360

static HANDLE g_hThread[3];
static DWORD g_dwThreadID[3];
static bool thread[3] = {false};
static BOOL dont_close = FALSE;
static HWND ModelGroupBox = NULL, FormatGroupBox = NULL, O3DSRadio = NULL, N3DSRadio = NULL, NANDList = NULL, InjectEmuButton = NULL, InjectRedButton = NULL, ExtractButton = NULL, BootBinButton = NULL, RefreshButton = NULL, StartFormatButton = NULL, NameTextbox = NULL, WriteNameButton = NULL;
HWND EmuNANDDriveList = NULL, FormatDriveList = NULL, ProgressBar = NULL;

WPARAM wParamState = 0;

void ToggleCloseButton(HWND hwnd, BOOL bState)
{
	UINT dwExtra = (bState ? MF_ENABLED : (MF_DISABLED | MF_GRAYED));
	EnableMenuItem(GetSystemMenu(hwnd, FALSE), SC_CLOSE, MF_BYCOMMAND | dwExtra);
	dont_close = (bState ^ 1);
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

/* Taken from an example code made by ovidiucucu @ CodeGuru */
void CenterWindow(HWND hwnd)
{
	RECT rc, rcParent;
	int X, Y;
	HWND hwndParent = GetParent(hwnd);
	
	if (hwndParent == NULL) hwndParent = GetDesktopWindow();
	
	GetWindowRect(hwnd, &rc);
	GetWindowRect(hwndParent, &rcParent);
	X = (((rcParent.right - rcParent.left) / 2) - ((rc.right - rc.left) / 2));
	Y = (((rcParent.bottom - rcParent.top) / 2) - ((rc.bottom - rc.top) / 2));
	SetWindowPos(hwnd, NULL, X, Y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
}

DWORD WINAPI NANDNameProc(LPVOID lpParameter)
{
	thread[2] = true;
	HWND hWndMain = (HWND)lpParameter;
	
	wchar_t name[NAME_LENGTH] = {0};
	bool read = (LOWORD(wParamState) != IDB_EMUNANDNAME_BUTTON);
	
	/* Store input values for the operation */
	nandnum = (SendMessage(NANDList, CB_GETCURSEL, 0, 0) + 1); // The combo box index is zero-based
	n3ds = (SendMessage(N3DSRadio, BM_GETCHECK, 0, 0) == BST_CHECKED);
	
	if (!read)
	{
		/* Copy the string written by the user to nand_name */
		SendMessage(NameTextbox, WM_GETTEXT, NAME_LENGTH, (LPARAM)name);
		wcstombs(nand_name, name, NAME_LENGTH - 1);
	}
	
	if (WriteReadNANDName(hWndMain, read) < 0) goto out;
	
	if (read)
	{
		/* Display the string stored in the SD card */
		mbstowcs(name, nand_name, NAME_LENGTH - 1);
		SendMessage(NameTextbox, WM_SETTEXT, 0, (LPARAM)name);
	}
	
out:
	thread[2] = false;
	return 0;
}

bool isDriveListLocked(HWND List)
{
	wchar_t wstr[30] = {0};
	SendMessage(List, CB_GETLBTEXT, 0, (LPARAM)wstr);
	if (wcsncmp(wstr, L"No valid drives available", 25) == 0) return true;
	return false;
}

DWORD WINAPI DrivesProc(LPVOID lpParameter)
{
	thread[1] = true;
	HWND hWndMain = (HWND)lpParameter;
	
	/* Check if the MultiNANDProc thread is still running */
	bool nandThread = thread[0];
	if (!nandThread) EnableWindow(RefreshButton, FALSE);
	
	SendMessage(EmuNANDDriveList, CB_RESETCONTENT, 0, 0);
	SendMessage(FormatDriveList, CB_RESETCONTENT, 0, 0);
	
	if (ParseDrives(hWndMain) == -1)
	{
		SendMessage(EmuNANDDriveList, CB_ADDSTRING, 0, (LPARAM)L"No valid drives available");
		SendMessage(FormatDriveList, CB_ADDSTRING, 0, (LPARAM)L"No valid drives available");
		
		EnableWindow(EmuNANDDriveList, FALSE);
		EnableWindow(InjectEmuButton, FALSE);
		if (IsWindowEnabled(InjectRedButton)) EnableWindow(InjectRedButton, FALSE);
		EnableWindow(ExtractButton, FALSE);
		SendMessage(NameTextbox, WM_SETTEXT, 0, (LPARAM)L"\0");
		EnableWindow(NameTextbox, FALSE);
		EnableWindow(WriteNameButton, FALSE);
		
		EnableWindow(FormatDriveList, FALSE);
		EnableWindow(StartFormatButton, FALSE);
	} else {
		/* Verify that the combo boxes were properly populated */
		if (SendMessage(EmuNANDDriveList, CB_GETCOUNT, 0, 0) <= 0)
		{
			SendMessage(EmuNANDDriveList, CB_RESETCONTENT, 0, 0); // Just in case something really messed up
			SendMessage(EmuNANDDriveList, CB_ADDSTRING, 0, (LPARAM)L"No valid drives available");
			EnableWindow(EmuNANDDriveList, FALSE);
			EnableWindow(InjectEmuButton, FALSE);
			if (IsWindowEnabled(InjectRedButton)) EnableWindow(InjectRedButton, FALSE);
			EnableWindow(ExtractButton, FALSE);
			SendMessage(NameTextbox, WM_SETTEXT, 0, (LPARAM)L"\0");
			EnableWindow(NameTextbox, FALSE);
			EnableWindow(WriteNameButton, FALSE);
		} else {
			EnableWindow(EmuNANDDriveList, TRUE);
			EnableWindow(InjectEmuButton, TRUE);
			if (SendMessage(O3DSRadio, BM_GETCHECK, 0, 0) == BST_CHECKED) EnableWindow(InjectRedButton, TRUE);
			EnableWindow(ExtractButton, TRUE);
			EnableWindow(NameTextbox, TRUE);
			EnableWindow(WriteNameButton, TRUE);
		}
		
		if (SendMessage(FormatDriveList, CB_GETCOUNT, 0, 0) <= 0)
		{
			SendMessage(FormatDriveList, CB_RESETCONTENT, 0, 0); // Just in case something really messed up
			SendMessage(FormatDriveList, CB_ADDSTRING, 0, (LPARAM)L"No valid drives available");
			EnableWindow(FormatDriveList, FALSE);
			EnableWindow(StartFormatButton, FALSE);
		} else {
			EnableWindow(FormatDriveList, TRUE);
			EnableWindow(StartFormatButton, TRUE);
		}
	}
	
	SendMessage(EmuNANDDriveList, CB_SETCURSEL, 0, 0); // Sets the current selection to the first item
	SendMessage(FormatDriveList, CB_SETCURSEL, 0, 0); // Sets the current selection to the first item
	
	if (IsWindowEnabled(EmuNANDDriveList) && thread[2] == false)
	{
		/* Read the EmuNAND name */
		g_hThread[2] = CreateThread(NULL, 0, NANDNameProc, hWndMain, 0, &g_dwThreadID[2]);
	}
	
	if (!nandThread) EnableWindow(RefreshButton, TRUE);
	
	thread[1] = false;
	return 0;
}

DWORD WINAPI MultiNANDProc(LPVOID lpParameter)
{
	thread[0] = true;
	HWND hWndMain = (HWND)lpParameter;
	
	OPENFILENAME ofn;
	wchar_t filename[512] = {0};
	DWORD fpos = 0, fext = 0;
	
	ZeroMemory(&ofn, sizeof(ofn));
	
	ofn.lStructSize = sizeof(OPENFILENAME);
	ofn.hwndOwner = hWndMain;
	ofn.hInstance = NULL;
	ofn.lpstrFilter = TEXT("Nintendo 3DS NAND dump (*.bin)\0*.bin\0\0");
	ofn.lpstrCustomFilter = NULL;
	ofn.nMaxCustFilter = 0;
	ofn.nFilterIndex = 0;
	ofn.lpstrFile = filename;
	ofn.nMaxFile = 512;
	ofn.lpstrFileTitle = NULL;
	ofn.nMaxFileTitle = 0;
	ofn.lpstrInitialDir = NULL;
	ofn.lpstrTitle = (LOWORD(wParamState) == IDB_EXTRACT_BUTTON ? TEXT("Select the output NAND dump") : TEXT("Select an input NAND dump"));
	ofn.Flags = (LOWORD(wParamState) == IDB_EXTRACT_BUTTON ? (OFN_DONTADDTORECENT | OFN_NONETWORKBUTTON | OFN_CREATEPROMPT | OFN_OVERWRITEPROMPT) : (OFN_DONTADDTORECENT | OFN_NONETWORKBUTTON | OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST));
	ofn.nFileOffset = fpos;
	ofn.nFileExtension = fext;
	ofn.lpstrDefExt = TEXT("bin");
	ofn.lCustData = 0;
	ofn.lpfnHook = NULL;
	ofn.lpTemplateName = NULL;
	
	BOOL fopened = (LOWORD(wParamState) == IDB_EXTRACT_BUTTON ? GetSaveFileName(&ofn) : GetOpenFileName(&ofn));
	if (fopened)
	{
		/* Disable window controls */
		EnableWindow(O3DSRadio, FALSE);
		EnableWindow(N3DSRadio, FALSE);
		EnableWindow(NANDList, FALSE);
		EnableWindow(EmuNANDDriveList, FALSE);
		EnableWindow(NameTextbox, FALSE);
		EnableWindow(WriteNameButton, FALSE);
		EnableWindow(InjectEmuButton, FALSE);
		if (IsWindowEnabled(InjectRedButton)) EnableWindow(InjectRedButton, FALSE);
		EnableWindow(ExtractButton, FALSE);
		EnableWindow(BootBinButton, FALSE);
		EnableWindow(RefreshButton, FALSE);
		EnableWindow(FormatDriveList, FALSE);
		EnableWindow(StartFormatButton, FALSE);
		ToggleCloseButton(hWndMain, FALSE);
		
		/* Store input values for the operation */
		nandnum = (LOWORD(wParamState) == IDB_STARTFORMAT_BUTTON ? 1 : (SendMessage(NANDList, CB_GETCURSEL, 0, 0) + 1)); // The combo box index is zero-based
		n3ds = (SendMessage(N3DSRadio, BM_GETCHECK, 0, 0) == BST_CHECKED);
		is_input = (LOWORD(wParamState) != IDB_EXTRACT_BUTTON);
		cfw = (LOWORD(wParamState) == IDB_INJECTRED_BUTTON);
		
		/* Restart the progress bar */
		SendMessage(ProgressBar, PBM_SETPOS, 0, 0);
		
		/* Do the magic */
		InjectExtractNAND(ofn.lpstrFile, hWndMain, (LOWORD(wParamState) == IDB_STARTFORMAT_BUTTON ? true : false));
		
		/* Update the drop-down lists */
		if (LOWORD(wParamState) == IDB_STARTFORMAT_BUTTON) g_hThread[1] = CreateThread(NULL, 0, DrivesProc, hWndMain, 0, &g_dwThreadID[1]);
		
		/* Enable window controls */
		EnableWindow(O3DSRadio, TRUE);
		EnableWindow(N3DSRadio, TRUE);
		EnableWindow(NANDList, TRUE);
		EnableWindow(EmuNANDDriveList, TRUE);
		EnableWindow(NameTextbox, TRUE);
		EnableWindow(WriteNameButton, TRUE);
		EnableWindow(InjectEmuButton, TRUE);
		if (SendMessage(O3DSRadio, BM_GETCHECK, 0, 0) == BST_CHECKED) EnableWindow(InjectRedButton, TRUE);
		EnableWindow(ExtractButton, TRUE);
		EnableWindow(BootBinButton, TRUE);
		EnableWindow(RefreshButton, TRUE);
		if (LOWORD(wParamState) != IDB_STARTFORMAT_BUTTON && !isDriveListLocked(FormatDriveList))
		{
			EnableWindow(FormatDriveList, TRUE);
			EnableWindow(StartFormatButton, TRUE);
		}
		ToggleCloseButton(hWndMain, TRUE);
	}
	
	thread[0] = false;
	return 0;
}

bool CALLBACK SetFont(HWND child, LPARAM font)
{
	SendMessage(child, WM_SETFONT, (WPARAM)font, TRUE);
	return true;
}

// Window procedure for our main window.
LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	static HINSTANCE hInstance;
	static LOGFONT lf;
	static HFONT hFont;
	
	switch (msg)
	{
		case WM_CREATE:
		{
			/* Initialize current program instance */
			hInstance = GetModuleHandle(NULL);
			
			/* Create the "Nintendo 3DS Model" group box and the "Old 3DS" & "New 3DS" circle options */
			ModelGroupBox = CreateWindowEx(0, TEXT("BUTTON"), TEXT("Nintendo 3DS Model"), WS_VISIBLE | WS_CHILD | BS_GROUPBOX, 10, 30, 160, 50, hWnd, NULL, hInstance, NULL);
			O3DSRadio = CreateWindowEx(0, TEXT("BUTTON"), TEXT("Old 3DS"), WS_VISIBLE | WS_CHILD | BS_AUTORADIOBUTTON, 20, 50, 70, 20, hWnd, (HMENU)IDB_O3DS_RADIO, hInstance, NULL);
			N3DSRadio = CreateWindowEx(0, TEXT("BUTTON"), TEXT("New 3DS"), WS_VISIBLE | WS_CHILD | BS_AUTORADIOBUTTON, 90, 50, 70, 20, hWnd, (HMENU)IDB_N3DS_RADIO, hInstance, NULL);
			SendMessage(O3DSRadio, BM_SETCHECK, (WPARAM)BST_CHECKED, 0); // Checks the "Old 3DS" option
			
			/* Create the "NAND Number" drop-down list */
			NANDList = CreateWindowEx(0, TEXT("COMBOBOX"), NULL, WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | CBS_SORT, 190, 55, 100, 20, hWnd, (HMENU)IDB_NANDNUMBER_LIST, hInstance, NULL);
			for (int i = 1; i <= MAX_NAND_NUM; i++)
			{
				wchar_t num_str[2];
				_snwprintf(num_str, 2, L"%c\0", (i + 0x30)); // Base-10 integer to ANSI conversion
				SendMessage(NANDList, CB_ADDSTRING, 0, (LPARAM)num_str);
			}
			SendMessage(NANDList, CB_SETCURSEL, 0, 0); // Sets the current selection to the first item
			
			/* Create the "EmuNAND SD Card" drop-down list */
			EmuNANDDriveList = CreateWindowEx(0, TEXT("COMBOBOX"), NULL, WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | CBS_SORT, 130, 90, 160, 20, hWnd, (HMENU)IDB_EMUNANDDRIVE_LIST, hInstance, NULL);
			
			/* Create the "EmuNAND Name" textbox */
			NameTextbox = CreateWindowEx(WS_EX_CLIENTEDGE, TEXT("EDIT"), NULL, WS_VISIBLE | WS_CHILD | ES_LEFT | ES_AUTOHSCROLL, 130, 120, 160, 20, hWnd, (HMENU)IDB_EMUNANDNAME_TEXTBOX, hInstance, NULL);
			SendMessage(NameTextbox, EM_SETLIMITTEXT, NAME_LENGTH - 1, 0); // Limit text input
			
			/* Create the "Inject EmuNAND", "Inject RedNAND", "Extract NAND", "Modify boot.bin", "Write EmuNAND name" and "Refresh drives" buttons */
			InjectEmuButton = CreateWindowEx(0, TEXT("BUTTON"), TEXT("Inject EmuNAND"), WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 20, 150, 130, 20, hWnd, (HMENU)IDB_INJECTEMU_BUTTON, hInstance, NULL);
			InjectRedButton = CreateWindowEx(0, TEXT("BUTTON"), TEXT("Inject RedNAND"), WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 160, 150, 130, 20, hWnd, (HMENU)IDB_INJECTRED_BUTTON, hInstance, NULL);
			ExtractButton = CreateWindowEx(0, TEXT("BUTTON"), TEXT("Extract NAND"), WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 20, 180, 130, 20, hWnd, (HMENU)IDB_EXTRACT_BUTTON, hInstance, NULL);
			BootBinButton = CreateWindowEx(0, TEXT("BUTTON"), TEXT("Modify boot.bin"), WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 160, 180, 130, 20, hWnd, (HMENU)IDB_BOOTBIN_BUTTON, hInstance, NULL);
			WriteNameButton = CreateWindowEx(0, TEXT("BUTTON"), TEXT("Write EmuNAND name"), WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 20, 210, 130, 20, hWnd, (HMENU)IDB_EMUNANDNAME_BUTTON, hInstance, NULL);
			RefreshButton = CreateWindowEx(0, TEXT("BUTTON"), TEXT("Refresh drives"), WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 160, 210, 130, 20, hWnd, (HMENU)IDB_REFRESH_BUTTON, hInstance, NULL);
			
			/* Create the "Format EmuNAND" group box, the mapped drives drop-down list and the "Start Format" button */
			FormatGroupBox = CreateWindowEx(0, TEXT("BUTTON"), TEXT("Format EmuNAND"), WS_VISIBLE | WS_CHILD | BS_GROUPBOX, 10, 240, 295, 50, hWnd, NULL, hInstance, NULL);
			FormatDriveList = CreateWindowEx(0, TEXT("COMBOBOX"), NULL, WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | CBS_SORT, 20, 260, 160, 20, hWnd, NULL, hInstance, NULL);
			StartFormatButton = CreateWindowEx(0, TEXT("BUTTON"), TEXT("Start format"), WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 190, 260, 100, 20, hWnd, (HMENU)IDB_STARTFORMAT_BUTTON, hInstance, NULL);
			
			/* Create the progress bar */
			ProgressBar = CreateWindowEx(0, PROGRESS_CLASS, NULL, WS_VISIBLE | WS_CHILD | PBS_SMOOTH, 10, 300, 295, 20, hWnd, NULL, hInstance, NULL);
			SendMessage(ProgressBar, PBM_SETSTEP, 1, 0);
			
			/* Initialize font */
			GetObject(GetStockObject(DEFAULT_GUI_FONT), sizeof(LOGFONT), &lf);
			hFont = CreateFont(lf.lfHeight, lf.lfWidth, lf.lfEscapement, lf.lfOrientation, lf.lfWeight, lf.lfItalic, lf.lfUnderline, lf.lfStrikeOut, lf.lfCharSet, lf.lfOutPrecision, lf.lfClipPrecision, lf.lfQuality, lf.lfPitchAndFamily, lf.lfFaceName);
			EnumChildWindows(hWnd, (WNDENUMPROC)SetFont, (LPARAM)hFont);
			
			/* Thread process to populate the drop-down lists */
			g_hThread[1] = CreateThread(NULL, 0, DrivesProc, hWnd, 0, &g_dwThreadID[1]);
			
			/* Center the program window */
			CenterWindow(hWnd);
			
			break;
		}
		case WM_CLOSE:
			if (dont_close)
			{
				/* We can't close the program until the operation has been finished */
				return TRUE;
			} else {
				DestroyWindow(hWnd);
				break;
			}
		case WM_DESTROY:
			DeleteObject(hFont);
			PostQuitMessage(0);
			break;
		case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hWnd, &ps);
			SelectObject(hdc, hFont);
			SetTextColor(hdc, 0);
			SetBkMode(hdc, TRANSPARENT);
			TextOut(hdc, 190, 40, TEXT("NAND Number:"), 12);
			TextOut(hdc, 20, 95, TEXT("EmuNAND SD Card:"), 16);
			TextOut(hdc, 20, 125, TEXT("EmuNAND Name:"), 13);
			TextOut(hdc, 0, 0, TEXT(COPYRIGHT), GetTextSize(TEXT(COPYRIGHT)));
			TextOut(hdc, 290, 0, TEXT(VER_FILEVERSION_STR), GetTextSize(TEXT(VER_FILEVERSION_STR)));
			EndPaint(hWnd, &ps);
			break;
		}
		case WM_COMMAND:
			switch (LOWORD(wParam))
			{
				case IDB_N3DS_RADIO:
					if (IsWindowEnabled(InjectRedButton)) EnableWindow(InjectRedButton, FALSE);
					break;
				case IDB_O3DS_RADIO:
					if (!IsWindowEnabled(InjectRedButton) && !isDriveListLocked(EmuNANDDriveList)) EnableWindow(InjectRedButton, TRUE);
					break;
				case IDB_INJECTEMU_BUTTON:
				case IDB_INJECTRED_BUTTON:
				case IDB_EXTRACT_BUTTON:
				case IDB_STARTFORMAT_BUTTON:
					/* Copy the current wParam value */
					wParamState = wParam;
					
					/* Create process thread */
					g_hThread[0] = CreateThread(NULL, 0, MultiNANDProc, hWnd, 0, &g_dwThreadID[0]);
					break;
				case IDB_BOOTBIN_BUTTON:
				{
					OPENFILENAME ofn;
					wchar_t filename[512] = {0};
					DWORD fpos = 0, fext = 0;
					
					ZeroMemory(&ofn, sizeof(ofn));
					
					ofn.lStructSize = sizeof(OPENFILENAME);
					ofn.hwndOwner = hWnd;
					ofn.hInstance = NULL;
					ofn.lpstrFilter = TEXT("Palantine CFW boot.bin file (*.bin)\0*.bin\0\0");
					ofn.lpstrCustomFilter = NULL;
					ofn.nMaxCustFilter = 0;
					ofn.nFilterIndex = 0;
					ofn.lpstrFile = filename;
					ofn.nMaxFile = 512;
					ofn.lpstrFileTitle = NULL;
					ofn.nMaxFileTitle = 0;
					ofn.lpstrInitialDir = NULL;
					ofn.lpstrTitle = TEXT("Select the CFW boot.bin file to modify");
					ofn.Flags = (OFN_DONTADDTORECENT | OFN_NONETWORKBUTTON | OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST);
					ofn.nFileOffset = fpos;
					ofn.nFileExtension = fext;
					ofn.lpstrDefExt = TEXT("bin");
					ofn.lCustData = 0;
					ofn.lpfnHook = NULL;
					ofn.lpTemplateName = NULL;
					
					BOOL fopened = GetOpenFileName(&ofn);
					if (fopened)
					{
						nandnum = (SendMessage(NANDList, CB_GETCURSEL, 0, 0) + 1); // The index is zero-based
						ModifyBootBin(ofn.lpstrFile, hWnd);
					}
					
					break;
				}
				case IDB_REFRESH_BUTTON:
					/* Create process thread */
					g_hThread[1] = CreateThread(NULL, 0, DrivesProc, hWnd, 0, &g_dwThreadID[1]);
					break;
				case IDB_NANDNUMBER_LIST:
				case IDB_EMUNANDDRIVE_LIST:
				case IDB_EMUNANDNAME_BUTTON:
					if ((IsWindowEnabled(EmuNANDDriveList) && ((LOWORD(wParam) == IDB_NANDNUMBER_LIST || LOWORD(wParam) == IDB_EMUNANDDRIVE_LIST) && HIWORD(wParam) == CBN_SELCHANGE)) || LOWORD(wParam) == IDB_EMUNANDNAME_BUTTON)
					{
						/* Copy the current wParam value */
						wParamState = wParam;
						
						/* Create process thread */
						if (thread[2] == false) g_hThread[2] = CreateThread(NULL, 0, NANDNameProc, hWnd, 0, &g_dwThreadID[2]);
					}
					break;
				default:
					break;
			}
			break;
		default:
			break;
	}
	
	return DefWindowProc(hWnd, msg, wParam, lParam);
}

// Our application entry point.
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	/* Check if we have administrative privileges */
	if (!IsUserAdmin())
	{
		MessageBox(NULL, TEXT("Error: not running with administrative privileges.\nPlease make sure you run the program as an Admin and try again."), TEXT("Error"), MB_ICONERROR | MB_OK);
		return 0;
	}
	
	WNDCLASSEX wc;
	LPCTSTR MainWndClass = TEXT(PRODUCT_NAME);
	HWND hWnd;
	MSG msg;
	
	// Initialise common controls.
	INITCOMMONCONTROLSEX InitCtrlEx;
	InitCtrlEx.dwSize = sizeof(INITCOMMONCONTROLSEX);
	InitCtrlEx.dwICC = ICC_WIN95_CLASSES;
	InitCommonControlsEx(&InitCtrlEx);
	
	// Class for our main window.
	wc.cbSize        = sizeof(wc);
	wc.style         = 0;
	wc.lpfnWndProc   = &MainWndProc;
	wc.cbClsExtra    = 0;
	wc.cbWndExtra    = 0;
	wc.hInstance     = hInstance;
	wc.hIcon         = (HICON) LoadImage(hInstance, MAKEINTRESOURCE(IDI_APPICON), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_DEFAULTCOLOR | LR_SHARED);
	wc.hCursor       = (HCURSOR) LoadImage(NULL, IDC_ARROW, IMAGE_CURSOR, 0, 0, LR_SHARED);
	wc.hbrBackground = (HBRUSH) (COLOR_BTNFACE + 1);
	wc.lpszMenuName  = NULL;
	wc.lpszClassName = MainWndClass;
	wc.hIconSm       = (HICON) LoadImage(hInstance, MAKEINTRESOURCE(IDI_APPICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR | LR_SHARED);
	
	// Register our window classes, or error.
	if (!RegisterClassEx(&wc))
	{
		MessageBox(NULL, TEXT("Error registering window class."), TEXT("Error"), MB_ICONERROR | MB_OK);
		return 0;
	}
	
	// Create instance of main window.
	hWnd = CreateWindowEx(0, MainWndClass, MainWndClass, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, WINDOW_WIDTH, WINDOW_HEIGHT, NULL, NULL, hInstance, NULL);
	
	// Error if window creation failed.
	if (!hWnd)
	{
		MessageBox(NULL, TEXT("Error creating main window."), TEXT("Error"), MB_ICONERROR | MB_OK);
		return 0;
	}
	
	// Show window and force a paint.
	ShowWindow(hWnd, nCmdShow);
	UpdateWindow(hWnd);
	
	// Main message loop.
	while (GetMessage(&msg, NULL, 0, 0) > 0)
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	
	return (int) msg.wParam;
}
