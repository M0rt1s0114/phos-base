// Open the volume behind a drive letter, for the eject sequence.
//
// Only removable media and CD-ROMs are ejectable at all, so anything else
// answers INVALID_HANDLE_VALUE without trying. Removable media is opened for
// write as well, because it has to be locked and dismounted before it goes;
// a CD-ROM is read only.
//
// szVolumeName is the shared "\\.\X:" buffer, whose fourth character is the
// letter.
HANDLE OpenVolume(TCHAR cDriveLetter)
{
	TCHAR tzRoot[4] = TEXT("C:\\");
	DWORD dwAccess;

	tzRoot[0] = cDriveLetter;
	switch (MyGetDriveType(tzRoot))
	{
	case DRIVE_REMOVABLE:
		dwAccess = GENERIC_READ | GENERIC_WRITE;
		break;
	case DRIVE_CDROM:
		dwAccess = GENERIC_READ;
		break;
	default:
		return INVALID_HANDLE_VALUE;        // nothing there can be pushed out
	}

	szVolumeName[4] = cDriveLetter;
	return CreateFile(szVolumeName, dwAccess, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_EXISTING, 0, NULL);
}
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
// The volume handle is owned by whoever opened it; this is CloseHandle with a
// name that reads like the sequence it belongs to.
BOOL CloseVolume(HANDLE hVolume)
{
	return CloseHandle(hVolume);
}
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
// Lock the volume, retrying for up to LOCK_TIMEOUT. FSCTL_LOCK_VOLUME fails
// while anything still has the volume open, so a caller that has just shut
// something down needs the retries: the handle it closed may not be gone yet.
// The wait is spread over RETRY attempts rather than slept once.
#define LOCK_TIMEOUT  10000 // milliseconds, spread over RETRY attempts
BOOL LockVolume(HANDLE hVolume)
{
	DWORD dwSleep = LOCK_TIMEOUT / RETRY;

	for (int nTry = 0; nTry < RETRY; nTry++)
	{
		if (DeviceIoControl(hVolume, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, NULL, NULL))
		{
			return TRUE;
		}
		Sleep(dwSleep);
	}
	return FALSE;
}
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
// Take the volume out of the file system's hands. This is the step that makes it
// safe to pull the media out: the drive letter stops resolving.
BOOL DismountVolume(HANDLE hVolume)
{
	return DeviceIoControl(hVolume, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, NULL, NULL);
}
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
// Ask the device not to let the media be pulled by hand, or let it again. The
// eject sequence turns it on while it works and off just before it ejects, so a
// running eject cannot be interrupted by somebody pressing the button.
/////////////////////////////////////////////////////////////////////////////


// Is there media in the drive? IOCTL_DISK_CHECK_VERIFY is the call that answers
// that for a CD-ROM or a card reader, and it is what the eject sequence checks
// before trying to eject nothing.
BOOL MediaExist(HANDLE hVolume)
{
	return DeviceIoControl(hVolume, IOCTL_DISK_CHECK_VERIFY, NULL, 0, NULL, 0, NULL, NULL);
}

/////////////////////////////////////////////////////////////////////////////
// Eject the media. The same IOCTL is what the -cd / -usb forms of EJEC end at.
BOOL AutoEjectVolume(HANDLE hVolume)
{
	return DeviceIoControl(hVolume, IOCTL_STORAGE_EJECT_MEDIA, NULL, 0, NULL, 0, NULL, NULL);
}
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
BOOL EjectVolume(WCHAR cDriveLetter, BOOL UsbDriver)
{
	HANDLE hVolume;
	BOOL bEjected;

	// Open the volume behind the letter. Only removable media and CD-ROMs
	// answer with a handle; anything else is not ejectable in the first place.
	hVolume = OpenVolume(cDriveLetter);
	if (hVolume == INVALID_HANDLE_VALUE)
	{
		return FALSE;
	}

	// Nothing in the drive is a success, not a failure: there is nothing to
	// push out, which is what the caller asked for.
	if (!MediaExist(hVolume))
	{
		CloseVolume(hVolume);
		return TRUE;
	}

	// Take it away from the file system, then push it out. Either step can
	// fail while something still holds the media, and that is the failure the
	// retry loop in the caller exists for.
	if (!LockVolume(hVolume) || !DismountVolume(hVolume))
	{
		CloseVolume(hVolume);
		return FALSE;
	}
	bEjected = AutoEjectVolume(hVolume);

	// A stick keeps its drive letter after the media is out, so drop the DOS
	// device mapping as well. A CD-ROM's letter points at nothing useful
	// afterwards but is harmless, so it is left where it is.
	if (UsbDriver && bEjected)
	{
		TCHAR tzLetter[3] = TEXT("_:");

		tzLetter[0] = (TCHAR) cDriveLetter;
		bEjected = DefineDosDevice(DDD_REMOVE_DEFINITION, tzLetter, NULL);
	}
	CloseVolume(hVolume);
	return bEjected;
}
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
// The eject capability.
//
// ptzDrive names one drive ("D:"), or is NULL for "every drive the two flags
// allow".
//
// A drive that will not let go is retried uRetry times. bAsk is the tray
// menu's mode and only the tray menu's: a person clicked "eject" and is
// standing there, so asking "try again?" is a question they can answer. A
// script gets one try and an answer -- SPEC-3's "unattended never shows a
// dialog" means a prompt in this loop is a PE that waits for a click nobody
// will make (the audit's R-06).
HRESULT EjectDrives(PCTSTR ptzDrive, UINT uRetry, BOOL bCDROM, BOOL bUSB, BOOL bAsk)
{
	UINT uType;
	BOOL bExist = FALSE;
	BOOL bDone = FALSE;
	UINT i = 0;

	while (!bDone)
	{
		if (ptzDrive)
		{
			uType = MyGetDriveType((PTSTR) ptzDrive);
			if ((uType != DRIVE_REMOVABLE) && (uType != DRIVE_CDROM))
			{
				break;      // named, but nothing there can be pushed out
			}
			bExist = TRUE;
			bDone = EjectVolume(ptzDrive[0], (uType == DRIVE_REMOVABLE));
		}
		else
		{
			TCHAR tzDrives[MAX_PATH];	// four characters per drive letter, twenty-six letters

			GetLogicalDriveStrings(_NumOf(tzDrives), tzDrives);
			for (PTSTR p = tzDrives; *p; p += UStrLen(p) + 1)
			{
				uType = MyGetDriveType(p);
				if (((uType == DRIVE_REMOVABLE) && bUSB) || ((uType == DRIVE_CDROM) && bCDROM))
				{
					bExist = TRUE;
					bDone = EjectVolume(p[0], (uType == DRIVE_REMOVABLE));
				}
			}
		}

		if (!bExist)
		{
			break;      // no drive of the wanted kinds at all
		}
		if (bDone)
		{
			break;
		}
		if (uRetry > 0)
		{
			i++;
			if (i == uRetry)
			{
				break;      // out of tries
			}
		}
		else if (bAsk)
		{
			TCHAR tzAsk[MAX_NAME];
			TCHAR tzTitle[MAX_NAME];

			LoadString(g_hInst, IDS_RETRY_EJECT, tzAsk, _NumOf(tzAsk));
			LoadString(g_hInst, IDS_EJECT_ERR, tzTitle, _NumOf(tzTitle));
			if (MessageBox(s_hLogo, tzAsk, tzTitle, MB_YESNO | MB_ICONQUESTION) != IDYES)
			{
				break;      // the operator is done with it
			}
		}
		else
		{
			break;      // one try, and a script is never asked for a click
		}
	}
	return bDone ? S_OK : S_FALSE;
}

// The legacy front end for this was "EJEC [C-|U-|D:][#N]". Callers now take the
// capability directly: `disk.eject({drive, cdrom, usb, retry})` from a script,
// and EjectDrives from the tray menu, whose two items name the two kinds.
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
// Where the temporary folder is: HKCU\Environment first, because in a PE the
// process environment and the stored one are not always the same thing -- which
// is what the legacy command was working around. TEMP, then TMP if that one is
// not set. Either may be written with variables in it, so what comes back is
// expanded. FALSE means neither key held anything.
//
// When neither key is set there is nothing to empty, and the cleanup says so
// with S_FALSE rather than falling back to the Windows directory the way the
// reference does. Case 050 and known-differences.txt carry that difference.
BOOL StoredTempFolder(PTSTR ptzBuf, DWORD cchBuf)
{
	TCHAR tzStored[MAX_PATH] = {0};
	DWORD dwType = REG_SZ;
	DWORD dwSize = sizeof(tzStored);

	if (SHGetValue(HKEY_CURRENT_USER, Text_Environment, TEXT("TEMP"), &dwType, tzStored, &dwSize) != ERROR_SUCCESS)
	{
		dwSize = sizeof(tzStored);
		if (SHGetValue(HKEY_CURRENT_USER, Text_Environment, TEXT("TMP"), &dwType, tzStored, &dwSize) != ERROR_SUCCESS)
		{
			return FALSE;
		}
	}

	ExpandEnvironmentStrings(tzStored, ptzBuf, cchBuf);

	// "%s\*.*" is built from this a few lines down. An empty value makes that
	// "\*.*", the root of whatever drive is current, and a bare "X:" does the
	// same thing with a letter in front. Both are refused here rather than left
	// to the delete guard: the caller is asking for a folder, and there is no
	// folder to answer with.
	{
		PTSTR p = ptzBuf + UStrLen(ptzBuf);

		while ((p > ptzBuf) && ((p[-1] == '\\') || (p[-1] == '/')))
		{
			p--;
		}
		if (((p - ptzBuf) <= 2) || !UDirExist(ptzBuf))
		{
			ptzBuf[0] = 0;
			return FALSE;
		}
	}
	return TRUE;
}

// Empty the folder the environment says is temporary. The folder browser starts
// from it, which is why the path is parked in pzInitDirName -- briefly: the
// browser sets that global itself when it opens.
HRESULT CleanTempFolder(VOID)
{
	TCHAR tzPath[MAX_STR];
	TCHAR tzSpec[MAX_STR];

	// Nothing stored means nothing to empty, and this says so with S_FALSE. The
	// reference answers 0 there, because its own fallback reaches the Windows
	// directory -- so its "success" means it emptied X:\Windows. That is not
	// worth matching for a result code: in a PE whose chain has run the keys are
	// set and both answers are 0. Case 050 records the difference, and it is in
	// tools/testrig/known-differences.txt.
	if (!StoredTempFolder(tzPath, _NumOf(tzPath)))
	{
		return S_FALSE;
	}
	if (UDirExist(tzPath))
	{
		pzInitDirName = tzPath;
	}
	UStrPrint(tzSpec, TEXT("%s\\*.*"), tzPath);

	return FsDelete(tzSpec);
}

// The temporary folder, as the tray menu's two items.
//
// The legacy front end was "TEMP [@] [DELETE|SETTING]"; the two actions it named
// are here instead, behind a menu click, which is what "a person is at the
// machine" means -- so these two ask before they act. The "@" form that answered
// the confirmation on the caller's behalf retires with the dialect: a script that
// wants the folder emptied without a question calls fs.temp_clean().

// Report where the folder is, then ask before emptying it. The answer is what
// CleanTempFolder answered, or S_FALSE when the person said no or nothing is
// stored to empty.
HRESULT TempClearFolder(VOID)
{
	TCHAR tzTempPath[MAX_STR] = {0};
	TCHAR tzBoxText[MAX_STR];
	TCHAR tzTitle[MAX_NAME];
	TCHAR tzTempFolder[MAX_NAME];
	TCHAR tzClearTemp[MAX_NAME];

	if (!StoredTempFolder(tzTempPath, _NumOf(tzTempPath)))
	{
		TCHAR tzReadEnviFail[MAX_NAME];
		TCHAR tzError[MAX_NAME];

		LoadString(g_hInst, IDS_ReadEnviFail, tzReadEnviFail, _NumOf(tzReadEnviFail));
		LoadString(g_hInst, IDS_Error, tzError, _NumOf(tzError));
		MessageBox(s_hLogo, tzReadEnviFail, tzError, MB_OK | MB_ICONERROR);
		return S_FALSE;
	}

	LoadString(g_hInst, IDS_TempFolder, tzTempFolder, _NumOf(tzTempFolder));
	UStrPrint(tzBoxText, tzTempFolder, tzTempPath);
	LoadString(g_hInst, IDS_ClearTemp, tzClearTemp, _NumOf(tzClearTemp));
	// Bounded: the buffer already holds the prompt plus the stored TEMP value,
	// and that value is expanded into a buffer this size further up, so it can
	// fill the buffer on its own. Only the tail of the question is lost.
	UStrCatTo(tzBoxText, tzClearTemp);
	LoadString(g_hInst, IDS_ClearTempFolder, tzTitle, _NumOf(tzTitle));
	if (MessageBox(s_hLogo, tzBoxText, tzTitle, MB_YESNO | MB_ICONQUESTION) != IDYES)
	{
		return S_FALSE;
	}
	return CleanTempFolder();
}

// Ask for a new folder and point both environment keys at it. The store is what
// a process started later reads; the process environment is what this one and
// everything it starts reads now, so both are written.
HRESULT TempSetFolder(VOID)
{
	TCHAR tzTempPath[MAX_STR] = {0};
	TCHAR tzBoxText[MAX_STR];
	TCHAR tzTitle[MAX_NAME];
	TCHAR tzTempFolder[MAX_NAME];
	TCHAR tzReSet[MAX_NAME];
	TCHAR tzNewTempFolder[MAX_NAME];
	TCHAR tzNewPath[MAX_STR];
	PTSTR ptzChosen;
	HRESULT hResult = S_OK;

	// The question carries where the folder is now, which is the one piece of
	// context that makes "change it" a question the person can answer.
	tzBoxText[0] = 0;
	if (StoredTempFolder(tzTempPath, _NumOf(tzTempPath)))
	{
		pzInitDirName = tzTempPath;
		LoadString(g_hInst, IDS_TempFolder, tzTempFolder, _NumOf(tzTempFolder));
		UStrPrint(tzBoxText, tzTempFolder, tzTempPath);
	}
	LoadString(g_hInst, IDS_ReSetTempFolder, tzReSet, _NumOf(tzReSet));
	UStrCatTo(tzBoxText, tzReSet);
	LoadString(g_hInst, IDS_SetTempFolder, tzTitle, _NumOf(tzTitle));
	if (MessageBox(s_hLogo, tzReSet, tzTitle, MB_YESNO | MB_ICONQUESTION) != IDYES)
	{
		return S_FALSE;
	}

	LoadString(g_hInst, IDS_NewTempFolder, tzNewTempFolder, _NumOf(tzNewTempFolder));
	ptzChosen = ShellBrowseFolder(tzTempPath, TRUE, tzNewTempFolder);
	if (!ptzChosen)
	{
		return S_FALSE;     // nothing picked: nothing to change
	}
	// The picker can answer with a file; the folder it sits in is the answer.
	if (!UDirExist(ptzChosen))
	{
		UDirSplitPath(ptzChosen);
	}
	if (!UDirExist(ptzChosen))
	{
		return S_FALSE;
	}

	// bSystem writes the stored value for this user and for the machine, which is
	// what a process started later reads; the process environment is set here.
	UStrPrint(tzNewPath, TEXT("TEMP=%s"), ptzChosen);
	hResult = SetEnvironment(tzNewPath, TRUE, FALSE) + hResult;
	UStrPrint(tzNewPath, TEXT("TMP=%s"), ptzChosen);
	hResult = SetEnvironment(tzNewPath, TRUE, FALSE) + hResult;
	return hResult;
}
//////////////////////////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
// --- the image-mount capability --------------------------------------------
//
// MOUN is two commands wearing one name, and the first field says which one:
//
//   MOUN <wim>,<mount dir>[,<index>[,<scratch>]]  put an image up
//   MOUN <wim>                                   take one down, by image file
//   MOUN ,<mount dir>                            take one down, by mount dir
//
// WIMGAPI, sitting on the mount filter driver, takes either name for the same
// unmount call, so coming down is one call here too. What differs between the
// halves is the answer, and that is the reference's shape rather than a choice:
// going up says yes or no, coming down reports what the library complained
// about. Case 055 pins both halves.
/////////////////////////////////////////////////////////////////////////////
typedef BOOL (WINAPI* PWIMMountImage)(PTSTR pzMountPath,PTSTR pzWimFileName,DWORD dwImageIndex,PTSTR pzTempPath);
typedef BOOL (WINAPI* PWIMUnMountImage)(PTSTR pzMountPath,PTSTR pzWimFileName,DWORD dwImageIndex,BOOL bCommitChanges);
#define WimFltrService TEXT("WimFltr")

// The filter behind a WIM mount is a service, so it is asked for before the
// library is: without the driver running, nothing the library does can work.
static HMODULE LoadWimApi(VOID)
{
	TCHAR tzPath[MAX_PATH];
	HMODULE hLib;

	// WIMGAPI.DLL is named bare, so it is resolved to the system directory the
	// same way NativeCall resolves a bare name -- LoadLibrary alone starts with
	// the current directory, which for a boot script is the media it was read
	// from (audit F-05, the half that named this call site).
	if (!SystemLibraryPath(TEXT("WIMGAPI.DLL"), tzPath, _NumOf(tzPath)))
	{
		return NULL;
	}
	hLib = LoadLibraryEx(tzPath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);

	SetServiceState(WimFltrService, FALSE, FALSE);
	return hLib;
}

// Put the image up. The index is 1-based, the way the text form numbers them,
// and the scratch folder may be NULL, which lets the library pick one.
static BOOL WimMount(PTSTR ptzWimFile, PTSTR ptzMountPath, INT iIndex, PTSTR ptzScratch)
{
	HMODULE hLib = LoadWimApi();
	PWIMMountImage pMount;
	BOOL bUp = FALSE;

	if (hLib)
	{
		pMount = (PWIMMountImage) GetProcAddress(hLib, "WIMMountImage");
		if (pMount)
		{
			bUp = pMount(ptzMountPath, ptzWimFile, (DWORD) iIndex, ptzScratch);
		}
		FreeLibrary(hLib);
	}
	return bUp;
}

// Take it down. ptzImage is the image's file name or its mount folder - the
// library accepts either, and the caller only decides which string it has.
//
// bCommit says whether the changes written into the mounted image are kept. The
// text dialect had no way to ask (its unmount always discarded), which is part
// of why mounting and unmounting are two verbs here rather than one that decides
// by which field the caller left out.
static HRESULT WimUnmount(PTSTR ptzImage, BOOL bCommit)
{
	HMODULE hLib = LoadWimApi();
	PWIMUnMountImage pUnmount;
	HRESULT hResult = S_FALSE;

	if (hLib)
	{
		pUnmount = (PWIMUnMountImage) GetProcAddress(hLib, "WIMUnmountImage");
		if (pUnmount)
		{
			if (pUnmount(ptzImage, NULL, 0, bCommit))
			{
				hResult = S_OK;
			}
			else
			{
				// E_INVALIDARG, for every shape that does not take - the
				// reference's answer on this half (case 055 pins four of them).
				// It is not the library's own error being passed along: that
				// was tried, and the library says ERROR_BAD_FORMAT here while
				// the reference says ERROR_INVALID_PARAMETER.
				hResult = E_INVALIDARG;
			}
		}
		FreeLibrary(hLib);
	}
	return hResult;
}

// The capability itself: which of the three shapes this is comes down to which
// fields the caller filled in.
HRESULT MountWimImage(PCTSTR ptzImage, PCTSTR ptzMountPath, INT iIndex, PCTSTR ptzTempDir)
{
	if ((ptzImage == NULL) || (ptzImage[0] == 0))
	{
		// An empty image name asks for the unmount-by-mount-folder shape, and
		// that needs a folder. With neither field named there is nothing to
		// take down at all.
		if ((ptzMountPath == NULL) || (ptzMountPath[0] == 0))
		{
			return E_INVALIDARG;
		}
		return WimUnmount((PTSTR) ptzMountPath, FALSE);
	}
	if (ptzMountPath == NULL)
	{
		// No comma at all: the line names an image that is already up.
		return WimUnmount((PTSTR) ptzImage, FALSE);
	}
	return WimMount((PTSTR) ptzImage, (PTSTR) ptzMountPath, iIndex, (PTSTR) ptzTempDir)
		? S_OK : S_FALSE;
}

// The unmount capability, named for what it does. ptzName is either the image
// file or the folder it was mounted at; the library takes both.
HRESULT UnmountWimImage(PCTSTR ptzName, BOOL bCommit)
{
	if ((ptzName == NULL) || (ptzName[0] == 0))
	{
		return E_INVALIDARG;
	}
	return WimUnmount((PTSTR) ptzName, bCommit);
}

// MOUN <wim>,<mount dir>[,<index>[,<scratch dir>]]
//
// The fields are separated in place, so the command text has to be writable.
// The scratch folder is the one field with a rule of its own: it is dropped
// unless it names something that exists, because the library wants a folder it
// can put its files in and the text form lets the author leave the field empty.
// The reference wrote that rule as a pointer multiplied by the answer of a file
// test, which stops working once a pointer no longer fits in 32 bits.
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Execute command


//////////////////////////////////////////////////////////////////////////////////////////////////
// Drive letters
//
// SHOW, DISK and RECY all need the same two things: walk the volumes Windows
// knows about, and find a free drive letter. A partition that is not mounted
// anywhere has no name other than its \\?\Volume{...}\ path, which is what
// FindFirstVolume hands out, and giving it a letter is what mounting it means.
//
// A: and B: are skipped - the floppy letters, which Windows reports as in use
// whether or not there is a drive there. X: is the PE's own ramdisk and is in
// use anyway, so the "already used" test covers it.
//////////////////////////////////////////////////////////////////////////////////////////////////

// The drive letters a volume already answers to, as "C:;D:;".
static VOID VolumeLetters(PCTSTR ptzVolume, PTSTR ptzLetters, UINT uLen)
{
	TCHAR tzNames[MAX_PATH * 4];
	DWORD dwLen = 0;

	ptzLetters[0] = 0;
	if (!GetVolumePathNamesForVolumeName(ptzVolume, tzNames, _NumOf(tzNames), &dwLen))
	{
		return;
	}
	for (PTSTR p = tzNames; *p; p += UStrLen(p) + 1)
	{
		if ((p[1] == ':') && (UStrLen(ptzLetters) + 4 < uLen))
		{
			UStrCat(ptzLetters, p);
			UStrCat(ptzLetters, TEXT(";"));
		}
	}
}

static TCHAR NextFreeLetter(TCHAR cFrom)
{
	DWORD dwLogical = GetLogicalDrives();

	// The shift below is (c - 'A'), so the starting point has to be one of the
	// twenty-six. A caller that hands in anything else -- a lower-case letter, a
	// digit, a character from another alphabet -- gets the default instead of a
	// shift the hardware does not define. The verb refuses such a value before
	// this runs; this is the second line of defence, because the first one used
	// to live in a text command that is gone.
	if ((cFrom >= 'a') && (cFrom <= 'z'))
	{
		cFrom = (TCHAR) (cFrom - 'a' + 'A');
	}
	if ((cFrom < 'A') || (cFrom > 'Z'))
	{
		cFrom = 'C';
	}

	for (TCHAR c = cFrom; c <= 'Z'; c++)
	{
		if ((c == 'A') || (c == 'B'))
		{
			continue;
		}
		if (!(dwLogical & (1u << (c - 'A'))))
		{
			return c;
		}
	}
	return 0;
}

// Is this volume one of the partitions the machine keeps for itself -- the EFI
// system partition, the MSR, or a recovery partition?
//
// A letter on one of those is worse than useless: it shows up as a drive that
// opens to an error, and every tool that walks the letters starts poking at the
// firmware's own files. The reference hands them out anyway, which is what
// D-06 is about; the verbs that letter volumes skip them by default, and
// `disk.assign(id, letter)` is still there for somebody who means it.
static BOOL IsFirmwarePartition(PCTSTR ptzVolume)
{
	TCHAR tzOpen[MAX_PATH];
	size_t len;
	HANDLE h;
	PARTITION_INFORMATION_EX info;
	DWORD dwBytes = 0;
	BOOL bResult = FALSE;

	// CreateFile wants the volume name without its trailing backslash.
	if (!UStrCopyTo(tzOpen, ptzVolume))
	{
		return FALSE;
	}
	len = UStrLen(tzOpen);
	while ((len > 0) && (tzOpen[len - 1] == '\\'))
	{
		tzOpen[--len] = 0;
	}
	h = CreateFile(tzOpen, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
	{
		return FALSE;
	}
	if (DeviceIoControl(h, IOCTL_DISK_GET_PARTITION_INFO_EX, NULL, 0,
			&info, sizeof(info), &dwBytes, NULL))
	{
		if (info.PartitionStyle == PARTITION_STYLE_GPT)
		{
			static const GUID c_esp = { 0xc12a7328, 0xf81f, 0x11d2, { 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b } };
			static const GUID c_msr = { 0xe3c9e316, 0x0b5c, 0x4db8, { 0x81, 0x7d, 0xf9, 0x2d, 0xf0, 0x02, 0x15, 0xae } };
			static const GUID c_recovery = { 0xde94bba4, 0x06d1, 0x4d40, { 0xa1, 0x6a, 0xbf, 0xd5, 0x01, 0x79, 0xd6, 0xac } };

			bResult = (memcmp(&info.Gpt.PartitionType, &c_esp, sizeof(GUID)) == 0) ||
				(memcmp(&info.Gpt.PartitionType, &c_msr, sizeof(GUID)) == 0) ||
				(memcmp(&info.Gpt.PartitionType, &c_recovery, sizeof(GUID)) == 0);
		}
		else if (info.PartitionStyle == PARTITION_STYLE_MBR)
		{
			// 0xEF is the EFI system partition in an MBR table, 0x27 recovery.
			bResult = (info.Mbr.PartitionType == 0xEF) || (info.Mbr.PartitionType == 0x27);
		}
	}
	CloseHandle(h);
	return bResult;
}

// Give a drive letter to every volume that has none. A volume with no file
// system is skipped: a letter on it would be a drive that cannot be opened.
// Returns how many were mounted, or -1 when the volume list itself failed.
// The volume-lettering capability.
//
// Every volume Windows knows about but has no letter for gets the first free
// one at or after cFrom. The return is the number that were lettered, or -1
// when the volume list could not be read at all.
INT MountUnmountedVolumes(TCHAR cFrom)
{
	TCHAR tzVolume[MAX_PATH];
	HANDLE hFind;
	INT iCount = 0;

	hFind = FindFirstVolume(tzVolume, _NumOf(tzVolume));
	if (hFind == INVALID_HANDLE_VALUE)
	{
		return -1;
	}
	do
	{
		TCHAR tzLetters[MAX_PATH];
		TCHAR tzLabel[MAX_PATH];

		VolumeLetters(tzVolume, tzLetters, _NumOf(tzLetters));
		if (tzLetters[0])
		{
			continue;      // already has one - "-1" means do not move it
		}
		if (IsFirmwarePartition(tzVolume))
		{
			continue;      // the machine's own partitions do not want letters
		}
		if (!GetVolumeInformation(tzVolume, tzLabel, _NumOf(tzLabel), NULL, NULL, NULL, NULL, 0))
		{
			continue;
		}
		TCHAR cLetter = NextFreeLetter(cFrom);
		if (cLetter)
		{
			TCHAR tzRoot[4] = TEXT("C:\\");
			tzRoot[0] = cLetter;
			if (SetVolumeMountPoint(tzRoot, tzVolume))
			{
				iCount++;
			}
		}
	}
	while (FindNextVolume(hFind, tzVolume, _NumOf(tzVolume)));
	FindVolumeClose(hFind);
	return iCount;
}

// The volume GUID name alone: {xxxxxxxx-....}.
//////////////////////////////////////////////////////////////////////////////////////////////////
// Defined below, with the bus-type lookup it belongs to; declared here because
// the volume list wants it and sits above it.
static STORAGE_BUS_TYPE VolumeBusType(TCHAR cLetter);

// What volumes there are, as data rather than as a walk only this file can take.
//
// tzName is the name Windows itself uses for the volume (\\?\Volume{GUID}\).
// That is the identity to hand back later: a drive letter is a *mount point*, and
// it is the thing that changes. tzLetter is 0 for a volume that has none.
struct VolumeInfo
{
	TCHAR  tzName[MAX_PATH];
	TCHAR  tzLetter;
	TCHAR  tzLabel[MAX_PATH];
	UINT   uDriveType;			// DRIVE_* of its mount point, or DRIVE_NO_ROOT_DIR
	UINT   uBusType;			// STORAGE_BUS_TYPE, or BusTypeUnknown
	UINT64 uTotalMb;
	UINT64 uFreeMb;
};

// The volume this process is running from, by its Windows name.
//
// One thing must never be renamed: the volume the running system lives on. Every
// path a PE has is written in terms of that letter -- its system directory, its
// registry hives, the script that is asking -- so moving it saws off the branch
// the person is standing on. Refused here rather than left to the caller to
// remember.
static BOOL VolumeOfThisProgram(PTSTR ptzName, UINT uLen)
{
	TCHAR tzSelf[MAX_PATH];
	TCHAR tzRoot[MAX_PATH];

	if (!GetModuleFileName(NULL, tzSelf, _NumOf(tzSelf)) ||
		!GetVolumePathName(tzSelf, tzRoot, _NumOf(tzRoot)))
	{
		return FALSE;
	}
	return GetVolumeNameForVolumeMountPoint(tzRoot, ptzName, uLen) ? TRUE : FALSE;
}

// Put one letter on a volume, or take its letters off (cLetter == 0).
//
// The volume is named by its own id, not by a letter it happens to have: that is
// the whole point of pairing this with EnumVolumes. One volume, one letter --
// the letters it had are removed first, so "give this the U:" leaves nothing
// behind and `letter` in the volume record keeps meaning one thing.
//
// A letter another volume already holds is refused. The old FixUSB.exe took it;
// a script can see both volumes in disk.volumes() and say what should happen to
// the other one, which is one line and has no surprise in it.
HRESULT SetVolumeLetter(PCTSTR ptzVolume, TCHAR cLetter)
{
	TCHAR tzTarget[4] = TEXT("X:\\");
	TCHAR tzVolume[MAX_PATH];

	// The API wants the volume name with its trailing backslash, and so do the
	// three comparisons below. A caller that typed an id by hand without one used
	// to slip past "that letter belongs to somebody else" and past the guard on
	// the running system's own volume -- and then the letters it had would be
	// removed before SetVolumeMountPoint refused the name, leaving the volume
	// with no letter at all. Normalize once, here, and everything below sees the
	// same thing.
	if (!UStrCopyTo(tzVolume, ptzVolume))
	{
		return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
	}
	if (tzVolume[0] && (tzVolume[UStrLen(tzVolume) - 1] != '\\'))
	{
		if (!UStrCatTo(tzVolume, TEXT("\\")))
		{
			return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
		}
	}
	ptzVolume = tzVolume;

	if ((cLetter >= 'a') && (cLetter <= 'z'))
	{
		cLetter = (TCHAR) (cLetter - 'a' + 'A');
	}
	if (cLetter && ((cLetter < 'A') || (cLetter > 'Z')))
	{
		return E_INVALIDARG;
	}

	// The volume the running system is on keeps its letter. Asking for the letter
	// it already has is not a change and goes through; anything else is refused
	// before a single mount point is touched.
	{
		TCHAR tzSelfVolume[MAX_PATH];
		TCHAR tzCurrent[MAX_PATH];

		if (VolumeOfThisProgram(tzSelfVolume, _NumOf(tzSelfVolume)) &&
			(UStrCmpI(tzSelfVolume, ptzVolume) == 0))
		{
			VolumeLetters(ptzVolume, tzCurrent, _NumOf(tzCurrent));
			if (!((cLetter && (tzCurrent[0] == cLetter)) ||
				(!cLetter && (tzCurrent[0] == 0))))
			{
				return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
			}
		}
	}

	if (cLetter)
	{
		TCHAR tzHolder[MAX_PATH];

		tzTarget[0] = cLetter;
		if (GetVolumeNameForVolumeMountPoint(tzTarget, tzHolder, _NumOf(tzHolder)))
		{
			// Something is there. If it is this volume, the request is already
			// answered; if it is another one, this stops here and nothing has
			// been touched.
			if (UStrCmpI(tzHolder, ptzVolume) != 0)
			{
				return HRESULT_FROM_WIN32(ERROR_ALREADY_ASSIGNED);
			}
		}
	}

	{
		TCHAR tzLetters[MAX_PATH];

		VolumeLetters(ptzVolume, tzLetters, _NumOf(tzLetters));
		for (PTSTR p = tzLetters; p[0] && p[1]; p += 3)		// "C:;"
		{
			if ((p[0] == cLetter) && p[2] == ';')
			{
				continue;			// the one being asked for stays
			}
			{
				TCHAR tzOld[4] = TEXT("C:\\");

				tzOld[0] = p[0];
				if (!DeleteVolumeMountPoint(tzOld))
				{
					return (HRESULT) GetLastError();
				}
			}
		}
	}

	if (!cLetter)
	{
		return S_OK;				// "no letter" was the whole request
	}
	tzTarget[0] = cLetter;
	return SetVolumeMountPoint(tzTarget, ptzVolume) ? S_OK : (HRESULT) GetLastError();
}

BOOL EnumVolumes(std::vector<VolumeInfo> &out)
{
	TCHAR tzName[MAX_PATH];
	HANDLE hFind = FindFirstVolume(tzName, _NumOf(tzName));

	if (hFind == INVALID_HANDLE_VALUE)
	{
		return FALSE;
	}
	do
	{
		VolumeInfo info;
		TCHAR tzLetters[MAX_PATH];

		UMemSet(&info, 0, sizeof(info));
		UStrCopyN(info.tzName, tzName, _NumOf(info.tzName));

		// "C:;D:;" -> the first letter. A volume can have several; this verb
		// answers with one, and `letters` was not worth a second field until
		// somebody needs it.
		VolumeLetters(tzName, tzLetters, _NumOf(tzLetters));
		if (tzLetters[0])
		{
			info.tzLetter = tzLetters[0];
		}
		GetVolumeInformation(tzName, info.tzLabel, _NumOf(info.tzLabel),
			NULL, NULL, NULL, NULL, 0);

		if (info.tzLetter)
		{
			TCHAR tzRoot[4] = TEXT("X:\\");
			ULONGLONG uFree = 0;
			ULONGLONG uTotal = 0;
			ULONGLONG uAvailable = 0;

			tzRoot[0] = info.tzLetter;
			info.uDriveType = MyGetDriveType(tzRoot);
			info.uBusType = (UINT) VolumeBusType(info.tzLetter);
			if (GetDiskFreeSpaceEx(tzRoot, (PULARGE_INTEGER) &uAvailable,
				(PULARGE_INTEGER) &uTotal, (PULARGE_INTEGER) &uFree))
			{
				info.uTotalMb = uTotal / (1024 * 1024);
				info.uFreeMb = uFree / (1024 * 1024);
			}
		}
		else
		{
			// No letter, so there is no mount point to ask about. The volume is
			// still listed: "there is a volume here that nothing can reach" is a
			// thing a script may need to see.
			info.uDriveType = DRIVE_NO_ROOT_DIR;
			info.uBusType = (UINT) BusTypeUnknown;
		}
		out.push_back(info);
	}
	while (FindNextVolume(hFind, tzName, _NumOf(tzName)));
	FindVolumeClose(hFind);
	return TRUE;
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
static BOOL VolumeGuidName(PCTSTR ptzVolume, PTSTR ptzGuid, UINT uLen)
{
	PCTSTR ptzOpen = UStrChr(ptzVolume, '{');
	PCTSTR ptzClose = ptzOpen ? UStrChr(ptzOpen, '}') : NULL;
	UINT uGuid;

	if (!ptzOpen || !ptzClose)
	{
		return FALSE;
	}
	uGuid = (UINT) (ptzClose - ptzOpen) + 1;
	if (uGuid + 1 > uLen)
	{
		return FALSE;
	}
	UStrCopyN(ptzGuid, ptzOpen, uGuid + 1);
	return TRUE;
}
//////////////////////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////////////////////
// --- the volume the external tool mount hangs off ------------------------------
//
// disposition\FixUSB.exe U used to do this job: find the volume that really sits
// on a USB bus and give it the letter U:. That tool's rule was read back off the
// binary itself - it enumerates the drive letters, asks each device for its bus
// type with IOCTL_STORAGE_QUERY_PROPERTY, and finishes with SetVolumeMountPoint.
// Doing it here keeps the chain in one language, and it is the thing the
// external tool mount will be built on.
//
// Bus type, not DRIVE_REMOVABLE: a SATA disk in a tray is removable too, and a
// USB stick is not reported as DRIVE_REMOVABLE by every controller.
//////////////////////////////////////////////////////////////////////////////////////////////////
static STORAGE_BUS_TYPE VolumeBusType(TCHAR cLetter)
{
	TCHAR tzDevice[MAX_PATH];
	STORAGE_PROPERTY_QUERY query;
	BYTE abDesc[1024];
	DWORD dwBytes = 0;
	STORAGE_BUS_TYPE type = BusTypeUnknown;
	HANDLE h;

	// "\\\\.\\X:" - the trailing colon is part of the name, the letter is the
	// fifth character.
	UStrCopy(tzDevice, TEXT("\\\\.\\C:"));
	tzDevice[4] = cLetter;

	h = CreateFile(tzDevice, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
	{
		return BusTypeUnknown;
	}
	UMemSet(&query, 0, sizeof(query));
	query.PropertyId = StorageDeviceProperty;
	query.QueryType = PropertyStandardQuery;
	if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
			abDesc, sizeof(abDesc), &dwBytes, NULL))
	{
		type = ((PSTORAGE_DEVICE_DESCRIPTOR) abDesc)->BusType;
	}
	CloseHandle(h);
	return type;
}

// Mount the volume sitting on `ptzBus` as `cLetter`.
//
// S_OK when that volume answers to the letter afterwards, including when it
// already did. The volume keeps any other letters it had: this adds a mount
// point, it does not move one. That is a deliberate difference from
// FixUSB.exe, which removes the old letter first - an extra letter is cosmetic,
// and silently unmounting somebody's volume is not.
//
// Only volumes that already have a letter can be found this way, which is the
// same limit FixUSB.exe has. Giving a letter to a volume that has none needs a
// partition walk and belongs with the external tool mount.
HRESULT AssignLetterToBus(TCHAR cLetter, PTSTR ptzBus)
{
	TCHAR tzRoot[4] = TEXT("C:\\");
	TCHAR tzTarget[MAX_PATH];
	STORAGE_BUS_TYPE want;
	BOOL bFound = FALSE;
	TCHAR c;

	if (!UStrCmpI(ptzBus, TEXT("usb")))
	{
		want = BusTypeUsb;
	}
	else
	{
		return (HRESULT) ERROR_INVALID_PARAMETER;
	}
	if ((cLetter < 'C') || (cLetter > 'Z'))
	{
		return (HRESULT) ERROR_INVALID_PARAMETER;
	}

	for (c = 'C'; c <= 'Z'; c++)
	{
		TCHAR tzVolume[MAX_PATH];

		if (VolumeBusType(c) != want)
		{
			continue;
		}
		tzRoot[0] = c;
		if (GetVolumeNameForVolumeMountPoint(tzRoot, tzVolume, _NumOf(tzVolume)))
		{
			UStrCopy(tzTarget, tzVolume);
			bFound = TRUE;
			break;
		}
	}
	if (!bFound)
	{
		return (HRESULT) ERROR_FILE_NOT_FOUND;
	}

	tzRoot[0] = cLetter;
	{
		TCHAR tzHave[MAX_PATH];

		if (GetVolumeNameForVolumeMountPoint(tzRoot, tzHave, _NumOf(tzHave)) &&
			!UStrCmpI(tzHave, tzTarget))
		{
			return S_OK;		// it already answers to this letter
		}
	}
	if (!SetVolumeMountPoint(tzRoot, tzTarget))
	{
		return (HRESULT) GetLastError();
	}
	return S_OK;
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// SHOW - display partitions
//
//   SHOW -1:-1   show every partition
//
// Implemented: the form the shipped configs use, "-1" for the partition - give
// a drive letter to each volume that has none, and leave the ones that already
// have a letter alone. The rest of the documented surface (hd:part,letter,
// removing a letter, Chinese characters as letters, the wait time, -check) is
// on the list in docs/COMMAND-BACKLOG.md.
//////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// DISK - disk operations
//
//   DISK [变量名],[磁盘号],[分区号],功能,[USB盘符表][,选项]
//   1 = assign a letter, 2 = release, 3 = reassign, 22 = first primary part
//
// Implemented: 1 and 3, which is "give the partitions that have no letter
// one". NOT implemented: 2, 22, and the half of 3 that reorders letters that
// are *already* assigned. That half can move a letter out from under a running
// PE - the disk the PE booted from, or its own ramdisk - so it needs a
// dedicated test disk and an explicit test before it is worth writing. Until
// then DISK reports success for 1 and 3 without touching assigned letters,
// which is the part of its job that is safe to do.
//////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// RECY - recycle bin capacity
//
//   RECY D:,0        no recycle bin on D:
//   RECY *:,0        no recycle bin anywhere
//
// NT6 keeps this per volume under BitBucket\Volume\<volume GUID>: MaxCapacity
// in megabytes, and NukeOnDelete when the bin is off (the value the shell
// actually reads to decide "delete, do not move to the bin").
//////////////////////////////////////////////////////////////////////////////////////////////////
#define RECY_KEY TEXT("Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\BitBucket\\Volume")
// The recycle-bin capability.
//
// The capacity is per volume, keyed by the volume's GUID name under HKCU, and
// 0 means "do not use the recycle bin at all" (which is what NukeOnDelete is
// for). A drive of "*" or "" means every volume.
HRESULT SetRecycleCapacity(PCTSTR ptzDrive, DWORD dwCapacity)
{
	TCHAR tzVolume[MAX_PATH];
	HANDLE hFind;
	BOOL bAll = ((ptzDrive == NULL) || (ptzDrive[0] == 0) || (ptzDrive[0] == '*'));
	UINT uDone = 0;
	HRESULT hResult = S_OK;

	hFind = FindFirstVolume(tzVolume, _NumOf(tzVolume));
	if (hFind == INVALID_HANDLE_VALUE)
	{
		return S_FALSE;
	}
	do
	{
		TCHAR tzLetters[MAX_PATH];
		TCHAR tzGuid[MAX_PATH];
		TCHAR tzKey[MAX_STR];
		DWORD dwNuke = dwCapacity ? 0 : 1;

		VolumeLetters(tzVolume, tzLetters, _NumOf(tzLetters));
		if (!bAll && (ptzDrive != NULL))
		{
			TCHAR tzWanted[4] = TEXT("C:");

			tzWanted[0] = ptzDrive[0];
			if (UStrCmpNI(tzLetters, tzWanted, 2) != 0)
			{
				continue;
			}
		}
		if (!VolumeGuidName(tzVolume, tzGuid, _NumOf(tzGuid)))
		{
			continue;
		}
		UStrPrint(tzKey, TEXT("%s\\%s"), RECY_KEY, tzGuid);
		{
			// Both writes are the job. A volume whose setting could not be
			// written is not a volume this call did anything for: counting it as
			// done would be the "silent success" the audit's R-05 was about --
			// and worse here than elsewhere, because the shell goes on reading
			// whatever was there before and nobody is told.
			LONG lResult = SHSetValue(HKEY_CURRENT_USER, tzKey, TEXT("MaxCapacity"),
				REG_DWORD, &dwCapacity, sizeof(dwCapacity));

			if (lResult == ERROR_SUCCESS)
			{
				lResult = SHSetValue(HKEY_CURRENT_USER, tzKey, TEXT("NukeOnDelete"),
					REG_DWORD, &dwNuke, sizeof(dwNuke));
			}
			if (lResult != ERROR_SUCCESS)
			{
				if (hResult == S_OK)
				{
					hResult = (HRESULT) lResult;
				}
				continue;
			}
		}
		uDone++;
	}
	while (FindNextVolume(hFind, tzVolume, _NumOf(tzVolume)));
	FindVolumeClose(hFind);

	if (hResult != S_OK)
	{
		return hResult;			// a write failed; not "no volume matched"
	}
	return uDone ? S_OK : S_FALSE;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
