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
BOOL PreventRemovalOfVolume(HANDLE hVolume, BOOL fPreventRemoval)
{
	PREVENT_MEDIA_REMOVAL stRemoval;

	stRemoval.PreventMediaRemoval = (BOOLEAN) fPreventRemoval;
	return DeviceIoControl(hVolume, IOCTL_STORAGE_MEDIA_REMOVAL, &stRemoval,
		sizeof(stRemoval), NULL, 0, NULL, NULL);
}
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
// The eject capability. Ejec() below is the text front end.
//
// ptzDrive names one drive ("D:"). NULL means "every drive the two flags
// allow", and a NON-NULL but empty name is a separate thing: Ejec() clears the
// letter of a named drive when the line also named a kind (C-/U-), which
// leaves the pointer in place over an empty string. The loop tests the
// pointer, not the text, so that case names a drive, finds nothing ejectable
// there and gives up -- which is the shape a boot configuration runs into.
//
// A drive that will not let go is retried: uRetry times when the text form
// said "#N", and by asking otherwise, because a drive still held open is
// usually something the person in front of the machine can close.
HRESULT EjectDrives(PCTSTR ptzDrive, UINT uRetry, BOOL bCDROM, BOOL bUSB)
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
			TCHAR tzDrives[MAX_NAME];

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
		else
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
	}
	return bDone ? S_OK : S_FALSE;
}

// EJEC - push media out. The shapes the text form takes:
//
//   EJEC             every drive that can be pushed out
//   EJEC C-          CD-ROMs only
//   EJEC U-          removable media only
//   EJEC D:          that drive, whatever kind it is
//   EJEC ...#N       try N times instead of asking
//
// C- and U- are looked for anywhere in the line rather than parsed off the
// front, because that is how they were always written: each one asks for its
// own kind, and a line with neither (or with both) asks for both. The pointer
// StrStrI answers is tested for presence, not turned into a BOOL: the low
// half of a 64-bit pointer is not an answer to "was it found".
HRESULT Ejec(CoreContext& ctx, PTSTR ptzCmd)
{
	UINT uRetry = 0;
	UINT uType;
	BOOL bCDROM = TRUE;
	BOOL bUSB = TRUE;
	PTSTR ptzDrive;
	PTSTR ptzCount;

	ptzCmd = UStrTrim(ptzCmd);
	UStrToUpper(ptzCmd);

	ptzCount = UStrChr(ptzCmd, '#');
	if (ptzCount)
	{
		*ptzCount = 0;
		uRetry = UStrToInt(ptzCount + 1);
	}

	if (ptzCmd[0])
	{
		bCDROM = (StrStrI(ptzCmd, TEXT("C-")) != NULL);
		bUSB = (StrStrI(ptzCmd, TEXT("U-")) != NULL);
	}

	// A named drive is the letter just before the colon.
	ptzDrive = UStrChr(ptzCmd, ':');
	if (ptzDrive)
	{
		ptzDrive--;
		uType = MyGetDriveType(ptzDrive);
		if (bCDROM || bUSB)
		{
			// The line already named the kind it wants, so the name is not what
			// matters: clear the letter and let every drive be walked.
			*ptzDrive = 0;      // every ejectable drive
		}
		if (uType == DRIVE_REMOVABLE)
		{
			bUSB = TRUE;        // the named stick
		}
		if (uType == DRIVE_CDROM)
		{
			bCDROM = TRUE;      // the named CD-ROM
		}
	}

	return EjectDrives(ptzDrive, uRetry, bCDROM, bUSB);
}
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
// Where the temporary folder is: HKCU\Environment first, because in a PE the
// process environment and the stored one are not always the same thing -- which
// is what the text form was working around. TEMP, then TMP if that one is
// not set. Either may be written with variables in it, so what comes back is
// expanded. FALSE means neither key held anything.
//
// When neither key is set there is nothing to empty, and the cleanup says so
// with S_FALSE rather than falling back to the Windows directory.
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
	// alternative answers 0 there, because its fallback reaches the Windows
	// directory -- so its "success" means it emptied X:\Windows. That is not worth
	// matching for a result code: in a PE whose chain has run, the keys are set and
	// both answers are 0.
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

// TEMP [@] [DELETE|SETTING]
//
// With no option it reports where the temporary folder is and stops there.
// "DELETE" empties it, and "SETTING" asks the shell's folder picker for a new
// one and writes that into both environment keys and the process.
//
// The leading "@" answers the confirmation on the user's behalf. That is what
// lets the DELETE path run with nobody at the machine; SETTING cannot be driven
// that way, because the picker it opens is a dialog of its own.
HRESULT Temp(CoreContext& ctx, PTSTR ptzCmd)
{
	TCHAR tzTempPath[MAX_STR] = {0};
	TCHAR tzTempBuf[MAX_PATH] = {0};
	TCHAR tzBoxText[MAX_STR] = {0};
	DWORD dwType = REG_SZ;
	DWORD dwSize = MAX_PATH;
	BOOL bQuiet;
	BOOL bDelete;
	BOOL bSetting;
	HRESULT hResult = S_OK;

	ptzCmd = UStrTrim(ptzCmd);
	bQuiet = (*ptzCmd == '@');
	if (bQuiet)
	{
		ptzCmd++;
	}

	// The option is checked before anything else, because a TEMP without one is
	// not this command at all: the text form answers E_INVALIDARG rather than
	// guessing. DELETE wins over SETTING, which is the order these two were
	// always read in.
	bDelete = (StrStrI(ptzCmd, TEXT("DELETE")) != NULL);
	bSetting = !bDelete && (StrStrI(ptzCmd, TEXT("SETTING")) != NULL);
	if (!bDelete && !bSetting)
	{
		return E_INVALIDARG;
	}

	if (SHGetValue(HKEY_CURRENT_USER, Text_Environment, TEXT("TEMP"), &dwType, &tzTempBuf[0], &dwSize) != ERROR_SUCCESS)
	{
		if (SHGetValue(HKEY_CURRENT_USER, Text_Environment, TEXT("TMP"), &dwType, &tzTempBuf[0], &dwSize) != ERROR_SUCCESS)
		{
			if (!bQuiet)
			{
				TCHAR ReadEnviFail[MAX_NAME];
				TCHAR pError[MAX_NAME];

				LoadString(g_hInst, IDS_ReadEnviFail, ReadEnviFail, MAX_NAME);
				LoadString(g_hInst, IDS_Error, pError, MAX_NAME);
				MessageBox(s_hLogo, ReadEnviFail, pError, MD5Str_Len);
			}
			return S_FALSE;
		}
	}

	ExpandEnvironmentStrings(tzTempBuf, tzTempPath, MAX_STR);
	if (UDirExist(tzTempPath))
	{
		pzInitDirName = tzTempPath;
	}

	// Both branches below append their own question to this.
	{
		TCHAR TempFolder[MAX_NAME];

		LoadString(g_hInst, IDS_TempFolder, TempFolder, MAX_NAME);
		UStrPrint(tzBoxText, TempFolder, tzTempPath);
	}

	if (bDelete)
	{
		TCHAR ClearTemp[MAX_NAME];
		TCHAR ClearTempFolder[MAX_NAME];

		LoadString(g_hInst, IDS_ClearTemp, ClearTemp, MAX_NAME);
		LoadString(g_hInst, IDS_ClearTempFolder, ClearTempFolder, MAX_NAME);
		UStrCat(tzBoxText, ClearTemp);
		if (bQuiet || (MessageBox(s_hLogo, tzBoxText, ClearTempFolder, 36) == 6))
		{
			hResult = CleanTempFolder();
		}
	}
	else
	{
		TCHAR ReSetTempFolder[MAX_NAME];
		TCHAR SetTempFolder[MAX_NAME];

		LoadString(g_hInst, IDS_ReSetTempFolder, ReSetTempFolder, MAX_NAME);
		LoadString(g_hInst, IDS_SetTempFolder, SetTempFolder, MAX_NAME);
		UStrCat(tzBoxText, ReSetTempFolder);
		if (MessageBox(s_hLogo, tzBoxText, SetTempFolder, 36) == 6)
		{
			TCHAR NewTempFolder[MAX_NAME];
			PTSTR pzNewPtah;

			LoadString(g_hInst, IDS_NewTempFolder, NewTempFolder, MAX_NAME);
			pzNewPtah = ShellBrowseFolder(tzTempPath, TRUE, NewTempFolder);
			if (pzNewPtah)
			{
				if (!UDirExist(pzNewPtah))
				{
					UDirSplitPath(pzNewPtah);
				}
				if (UDirExist(pzNewPtah))
				{
					// MAX_STR, not MAX_PATH: the line is three names and two
					// separators joined together, and the pieces are already
					// path-sized.
					TCHAR pzNewTempPath[MAX_STR];

					UStrPrint(pzNewTempPath, TEXT("%s\\%s\\Temp=%s"), hKCU, Text_Environment, pzNewPtah);
					Regi(g_ctx, pzNewTempPath);
					UStrPrint(pzNewTempPath, TEXT("%s\\%s\\Tmp=%s"), hKCU, Text_Environment, pzNewPtah);
					Regi(g_ctx, pzNewTempPath);
					UStrPrint(pzNewTempPath, TEXT("TEMP=%s"), pzNewPtah);
					hResult = SetEnvironment(pzNewTempPath, TRUE, FALSE);
					UStrPrint(pzNewTempPath, TEXT("TMP=%s"), pzNewPtah);
					hResult = SetEnvironment(pzNewTempPath, TRUE, FALSE) + hResult;
					hResult = SetEnvironment(TEXT(""), FALSE, FALSE) + hResult;
				}
			}
		}
	}

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
// halves is the answer, and that is the compatible shape rather than a choice:
// going up says yes or no, coming down reports what the library complained
// about.
/////////////////////////////////////////////////////////////////////////////
typedef BOOL (WINAPI* PWIMMountImage)(PTSTR pzMountPath,PTSTR pzWimFileName,DWORD dwImageIndex,PTSTR pzTempPath);
typedef BOOL (WINAPI* PWIMUnMountImage)(PTSTR pzMountPath,PTSTR pzWimFileName,DWORD dwImageIndex,BOOL bCommitChanges);
#define WimFltrService TEXT("WimFltr")

// The filter behind a WIM mount is a service, so it is asked for before the
// library is: without the driver running, nothing the library does can work.
static HMODULE LoadWimApi(VOID)
{
	HMODULE hLib = LoadLibrary(TEXT("WIMGAPI.DLL"));

	Serv(g_ctx, WimFltrService);
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
// library accepts either, and the front end only decides which string it has.
static HRESULT WimUnmount(PTSTR ptzImage)
{
	HMODULE hLib = LoadWimApi();
	PWIMUnMountImage pUnmount;
	HRESULT hResult = S_FALSE;

	if (hLib)
	{
		pUnmount = (PWIMUnMountImage) GetProcAddress(hLib, "WIMUnmountImage");
		if (pUnmount)
		{
			if (pUnmount(ptzImage, NULL, 0, FALSE))
			{
				hResult = S_OK;
			}
			else
			{
				// E_INVALIDARG, for every shape that does not take. It is not
				// the library's own error being passed along: the library says
				// ERROR_BAD_FORMAT here while the text form says
				// ERROR_INVALID_PARAMETER.
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
		return WimUnmount((PTSTR) ptzMountPath);
	}
	if (ptzMountPath == NULL)
	{
		// No comma at all: the line names an image that is already up.
		return WimUnmount((PTSTR) ptzImage);
	}
	return WimMount((PTSTR) ptzImage, (PTSTR) ptzMountPath, iIndex, (PTSTR) ptzTempDir)
		? S_OK : S_FALSE;
}

// MOUN <wim>,<mount dir>[,<index>[,<scratch dir>]]
//
// The fields are separated in place, so the command text has to be writable.
// The scratch folder is the one field with a rule of its own: it is dropped
// unless it names something that exists, because the library wants a folder it
// can put its files in and the field may be left empty.
HRESULT Moun(CoreContext& ctx, PTSTR ptzCmd)
{
	INT iIndex = 1;
	PTSTR ptzScratch = NULL;
	PTSTR ptzImage = ptzCmd;
	PTSTR ptzMountPath = UStrChr(ptzImage, ',');

	if (ptzMountPath)
	{
		PTSTR ptzIndex;

		*ptzMountPath++ = 0;
		ptzIndex = UStrChr(ptzMountPath, ',');
		if (ptzIndex)
		{
			PTSTR ptzRest;

			*ptzIndex++ = 0;
			iIndex = UStrToInt(ptzIndex);
			ptzRest = UStrChr(ptzIndex, ',');
			if (ptzRest)
			{
				*ptzRest++ = 0;
				if (UFileExist(ptzRest))
				{
					ptzScratch = ptzRest;
				}
			}
		}
	}
	ptzImage = UStrTrim(ptzImage);
	ptzMountPath = UStrTrim(ptzMountPath);
	ptzScratch = UStrTrim(ptzScratch);
	return MountWimImage(ptzImage, ptzMountPath, iIndex, ptzScratch);
}
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

// Give a drive letter to every volume that has none. A volume with no file
// system is skipped: a letter on it would be a drive that cannot be opened.
// Returns how many were mounted, or -1 when the volume list itself failed.
// The volume-lettering capability. Disk() below is the text front end,
// and so is the SHOW path that assigns a letter to one named volume.
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
// --- bus type of the volume behind a drive letter ------------------------------
//
// Bus type, not DRIVE_REMOVABLE: a SATA disk in a tray is removable too, and a
// USB stick is not reported as DRIVE_REMOVABLE by every controller. Asking the
// device itself, with IOCTL_STORAGE_QUERY_PROPERTY, is what tells them apart.
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
// point, it does not move one. Removing the old letter first would be cosmetic,
// and silently unmounting somebody's volume is not.
//
// Only volumes that already have a letter can be found this way, which is the
// limit of walking the drive letters. Giving a letter to a volume that has none
// needs a partition walk instead.
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
//   SHOW -1:-1            give a letter to every partition that has none
//   SHOW -from:<letter>   start the search at that letter
//
// The volumes that already have a letter are left alone. Only "-from:" is read
// out of the arguments; the partition field is accepted and ignored.
//////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT Show(CoreContext& ctx, PTSTR ptzCmd)
{
	TCHAR cFrom = 'C';
	PTSTR p = ptzCmd;

	while ((p = UStrChr(p, '-')) != NULL)
	{
		if (!UStrCmpNI(p, TEXT("-from:"), 6))
		{
			cFrom = p[6];
			break;
		}
		p++;
	}
	MountUnmountedVolumes(cFrom);
	return S_OK;
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// DISK - disk operations
//
//   DISK <variable>,<disk>,<partition>,<function>,[USB letters],[options]
//   1 = assign a letter, 2 = release, 3 = reassign, 22 = first primary part
//
// Functions 1 and 3 are read as "give the partitions that have no letter one".
// A letter that is already assigned is never touched: moving one out from under
// a running PE can take the disk it booted from, or its own ramdisk, with it.
// Any other function answers S_FALSE without acting.
//////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT Disk(CoreContext& ctx, PTSTR ptzCmd)
{
	INT iFunction = 0;
	PTSTR p = ptzCmd;

	for (UINT i = 0; (i < 4) && p; i++)
	{
		PTSTR q = UStrChr(p, ',');
		if (q)
		{
			*q = 0;
		}
		if (i == 3)
		{
			iFunction = UStrToInt(UStrTrim(p));
		}
		p = q ? (q + 1) : NULL;
	}

	if ((iFunction == 1) || (iFunction == 3))
	{
		if (MountUnmountedVolumes('C') < 0)
		{
			return S_FALSE;
		}
		return S_OK;
	}
	return S_FALSE;
}
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
// The recycle-bin capability. Recy() below is the text front end.
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
		SHSetValue(HKEY_CURRENT_USER, tzKey, TEXT("MaxCapacity"), REG_DWORD, &dwCapacity, sizeof(dwCapacity));
		SHSetValue(HKEY_CURRENT_USER, tzKey, TEXT("NukeOnDelete"), REG_DWORD, &dwNuke, sizeof(dwNuke));
		uDone++;
	}
	while (FindNextVolume(hFind, tzVolume, _NumOf(tzVolume)));
	FindVolumeClose(hFind);

	return uDone ? S_OK : S_FALSE;
}

HRESULT Recy(CoreContext& ctx, PTSTR ptzCmd)
{
	PTSTR ptzDrive = ptzCmd;
	PTSTR ptzSize = UStrChr(ptzCmd, ',');

	if (!ptzSize)
	{
		return E_INVALIDARG;
	}
	*ptzSize++ = 0;
	ptzDrive = UStrTrim(ptzDrive);

	return SetRecycleCapacity(ptzDrive, (DWORD) UStrToInt(UStrTrim(ptzSize)));
}
//////////////////////////////////////////////////////////////////////////////////////////////////
