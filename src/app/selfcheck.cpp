// Put one of the encoded string tables below back into text. Each character is
// stored as a WORD with an index-dependent offset, so this is the whole decoder;
// it is not encryption, it only keeps the strings out of a naive binary scan.
PTSTR UINT16ToUStr(PTSTR ptzBuf, const WORD ByteArray[], WORD pzBufLen)
{
	for (WORD i = 0; i < pzBufLen; i++)
	{
		ptzBuf[i] = ByteArray[i] + i + 1;
	}
	ptzBuf[pzBufLen] = 0;
	return ptzBuf;
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////

// Put the text on the clipboard as ANSI text, which is the format its readers
// ask for. Two details matter: the handle given to SetClipboardData belongs to
// the clipboard afterwards and must not be freed here, and a clipboard that
// would not empty is a failure rather than a success.
HRESULT SetClipboardText(PTSTR ptzText)
{
	CHAR szText[MAX_STR];
	HGLOBAL hGlobal;
	UINT uSize;
	BOOL bResult = FALSE;

	UStrToAStr(szText, ptzText, _NumOf(szText));
	uSize = UAStrLen(szText);

	if (!OpenClipboard(NULL))
	{
		return FALSE;
	}
	if (EmptyClipboard())
	{
		hGlobal = GlobalAlloc(GMEM_ZEROINIT, uSize + 1);
		if (hGlobal)
		{
			LPSTR pszLocked = (LPSTR) GlobalLock(hGlobal);

			if (pszLocked)
			{
				UMemCopy(pszLocked, szText, uSize + 1);
				GlobalUnlock(hGlobal);
				if (SetClipboardData(CF_TEXT, hGlobal) != NULL)
				{
					bResult = TRUE;         // the clipboard owns the handle now
				}
				else
				{
					GlobalFree(hGlobal);
				}
			}
			else
			{
				GlobalFree(hGlobal);
			}
		}
	}
	CloseClipboard();
	return bResult;
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
// DRIVE_REMOVABLE is what the eject path can work with, and a fixed disk that
// reports itself as hot-pluggable -- a tray, a USB bridge that claims to be fixed
// -- has to count as removable too. That is the question GetDriveType cannot
// answer, and the device's own hotplug info can.
//
// The device path buffer is shared with volume.cpp's OpenVolume: it is the same
// "\\.\X:" the eject sequence opens, so it lives out here.
TCHAR szVolumeName[8] = TEXT("\\\\.\\C:");

UINT MyGetDriveType(PTSTR ptzDriver)
{
	UINT uDriveType;
	HANDLE hDevice;

	if (!ptzDriver)
	{
		return NULL;
	}

	uDriveType = GetDriveType(ptzDriver);
	if ((uDriveType != DRIVE_FIXED) && (uDriveType != DRIVE_REMOVABLE))
	{
		return uDriveType;
	}

	szVolumeName[4] = *ptzDriver;
	hDevice = CreateFile(szVolumeName, GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (hDevice == INVALID_HANDLE_VALUE)
	{
		return uDriveType;
	}

	{
		STORAGE_HOTPLUG_INFO stHotplug;
		DWORD dwBytes = 0;

		if (DeviceIoControl(hDevice, IOCTL_STORAGE_GET_HOTPLUG_INFO, NULL, 0,
				&stHotplug, sizeof(stHotplug), &dwBytes, NULL) && stHotplug.DeviceHotplug)
		{
			uDriveType = DRIVE_REMOVABLE;
		}
	}
	CloseHandle(hDevice);
	return uDriveType;
}
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// Log result
// One line of the trace channel: formatted from a resource string, and only when
// a channel is open. The text is appended to the buffer the channel writes out.
VOID XLog(UINT uFmtRes, ...)
{
	va_list va;
	TCHAR tzLog[MAX_STR];
	TCHAR tzStr[MAX_PATH];

	if (!g_hXLog)
	{
		return;
	}
	va_start(va, uFmtRes);
	UStrPrintV(tzLog, _GetStr(uFmtRes), va);
	va_end(va);
	UStrCat(LogFileData, tzLog);
}
//////////////////////////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
#define UserKey TEXT("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion")
// The splash's background colour, as Winlogon stores it: "R G B" in one string
// under ...\Winlogon\Background. A value that is absent or half written answers
// black rather than reading whatever was on the stack.
DWORD GetBackgroundColor(void)
{
	// Zeroed on purpose: SHGetValue leaves the buffer untouched when the value
	// is absent, and the code below walks it looking for a space.
	TCHAR tzValue[MAX_NAME] = {0};
	TCHAR tzKey[MAX_NAME];
	DWORD dwType = REG_SZ;
	DWORD dwSize = sizeof(tzValue);
	CHAR cR = 0;
	CHAR cG = 0;
	CHAR cB = 0;
	PTSTR ptzPart;

	UStrPrint(tzKey, TEXT("%s\\Winlogon"), UserKey);
	SHGetValue(HKEY_LOCAL_MACHINE, tzKey, TEXT("Background"), &dwType, &tzValue[0], &dwSize);

	cR = (CHAR) UStrToInt(tzValue);
	ptzPart = UStrChr(tzValue, ' ');
	if (ptzPart)
	{
		*ptzPart++ = 0;
		cG = (CHAR) UStrToInt(ptzPart);
		ptzPart = UStrChr(ptzPart, ' ');
		if (ptzPart)
		{
			*ptzPart++ = 0;
			cB = (CHAR) UStrToInt(ptzPart);
		}
	}
	return RGB(cR, cG, cB);
}
/////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
//  Set Windows Registry
#define hKCU TEXT("HKCU")
