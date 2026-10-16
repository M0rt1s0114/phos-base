// Put this module's own help entry into the Start Menu. The label comes from the
// HelpLabel and the target is this executable, so the entry is a link to us; the
// label's %StartMenu% is expanded on the way.
//
// The shortcut is made through the capability rather than through the text LINK
// this used to spell out: that form packed the two pieces into one
// "<shortcut>,<target>" line.
///////////////////////////////////////////////////////////////////////////////


///////////////////////////////////////////////////////////////////////////////
// Get User SHELL
//////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// The shell-registration capability.
//
// This deliberately does NOT start the shell. MainThread already exists for
// that job: it waits for this registry value and then keeps whatever it names
// alive, restarting it whenever it goes missing. Starting it here as well looks
// harmless and is not -- that thread's loop leaves as soon as it sees the shell
// running, so a shell started here that dies later is never brought back. In
// the shipped image the driver install in Post\Inidrive.phos kills it, and the
// desktop never appears.
HRESULT SetSystemShell(PCTSTR ptzShell)
{
	// Upstream wrote the shell path only when both flags were set, by folding
	// them into the pointer with DWORD arithmetic. That truncates the address on
	// x64, and a NULL data pointer is not the same thing as not writing:
	// SHSetValue has no idea the caller meant "nothing".
	BOOL bSetShell;
	bSetShell = bRealBoot;

	return (SHSetValue(HKEY_LOCAL_MACHINE,
		PHOS_STATE,
		PESHELL,
		REG_SZ,
		bSetShell ? (void *) ptzShell : NULL,
		bSetShell ? (UStrLen((PTSTR) ptzShell) + 1) * sizeof(TCHAR) : 0) == ERROR_SUCCESS) ? S_OK : S_FALSE;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
// Load every font a spec matches. The spec is what FindFirstFile takes, so by the
// time it arrives it is a folder plus a wildcard; the names come back relative to
// that folder, so the folder is split off first.
//
// The answer is 0 whenever anything was looked at. The accumulator is the reason:
// it starts at S_FALSE rather than at zero, and AddFontResource never subtracts,
// so "found nothing to load" comes out as 0 as well. Case 051 pins the three
// answers this shape produces -- a folder of real fonts, a folder that is not
// there at all, and the drive-walking form -- as 0, 2 and 0.
HRESULT RegFont(PTSTR tzPath)
{
	WIN32_FIND_DATA fd;
	TCHAR tzFolder[MAX_PATH];
	HRESULT hResult = S_FALSE;
	HANDLE hFind;

	// The caller passes a spec that fits MAX_PATH, and a folder that does not
	// fit is refused rather than cut down: a shortened folder names a different
	// place to look for fonts.
	if (!UStrCopyTo(tzFolder, tzPath))
	{
		return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
	}
	UDirSplitPath(tzFolder);

	hFind = FindFirstFile(tzPath, &fd);
	if (hFind == INVALID_HANDLE_VALUE)
	{
		return (hResult == 0) ? S_FALSE : S_OK;
	}

	do
	{
		if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
		{
			TCHAR szFullPathName[MAX_PATH];

			UStrPrint(szFullPathName, TEXT("%s\\%s"), tzFolder, fd.cFileName);
			hResult = AddFontResource(szFullPathName) + hResult;
		}
	}
	while (FindNextFile(hFind, &fd));

	FindClose(hFind);
	SendMessage(HWND_BROADCAST, WM_FONTCHANGE, 0, 0);

	return (hResult == 0) ? S_FALSE : S_OK;
}
//////////////////////////////////////////////////////////////////////////////////////////


// The legacy front end for this was "FONT <folder|spec>", and its second form
// ("FONT \SubPath[,drive]") walked every drive for <drive>\SubPath\Fonts. That
// search left with the text dialect: `sys.font(path)` takes a file or a folder,
// and a script that wants the drive walk names the drives itself.
////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// Set WallPage
// The wallpaper capability.
//
// bRealBoot gates the data pointer: it is the "is this the author's own
// binary" check, and when it is false the value is written with a NULL data
// pointer. That is the reference's behaviour; nothing in the boot chain
// depends on the difference, because bRealBoot is true for our build.
HRESULT SetWallpaper(PCTSTR ptzPath)
{
	if (!UFileExist((PTSTR) ptzPath))
	{
		return S_FALSE;
	}
	// The stored value is what the desktop shows on the *next* logon; the call
	// below is what shows it now. A machine where the write fails still gets its
	// wallpaper, so the result is not checked -- deliberately, and this comment
	// is the record of that being a decision rather than an oversight.
	SHSetValue(HKEY_CURRENT_USER, TEXT("Control Panel\\Desktop"), TEXT("Wallpaper"),
		REG_SZ, bRealBoot ? (const void *) ptzPath : NULL,
		bRealBoot ? (UStrLen((PTSTR) ptzPath) + 1) * sizeof(TCHAR) : 0);
	SystemParametersInfo(SPI_SETDESKWALLPAPER, 0, NULL, SPIF_SENDWININICHANGE | SPIF_UPDATEINIFILE);
	return S_OK;
}



// The legacy front end for this was "NUMK <0|1>". The capability is
// `sys.numlock(on)`, which reads the keyboard and presses the key only when the
// state differs -- pressing it when it is already right would flip it the other
// way. Case 047 pins the answer.
//////////////////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////////////////
// The registered owner, the organisation and the machine-wide Run key were
// capabilities of their own for a while. SPEC-10 retired the verbs: each of them
// is one `reg.set` on a key the caller can name (docs/SPEC-DECISIONS.md,
// "准入判据"), so three functions and two verbs became the registry verb the
// caller already had.
//////////////////////////////////////////////////////////////////////////////////////////////////


///////////////////////////////////////////////////
///////////////////////////////////////////////////


///////////////////////////////////////////////////
///////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// Start or stop a device: fill in the property-change request the class
// installer acts on (this device's own configuration profile, and the state
// that was asked for), hand it over, and let the installer do it.
static BOOL ManageDriver(HDEVINFO hDev, SP_DEVINFO_DATA& sDev, DWORD dwAction)
{
	SP_CLASSINSTALL_HEADER header;
	SP_PROPCHANGE_PARAMS sProp;

	header.cbSize = sizeof(header);
	header.InstallFunction = DIF_PROPERTYCHANGE;

	ZeroMemory(&sProp, sizeof(sProp));
	sProp.ClassInstallHeader = header;
	sProp.StateChange = dwAction;                       // start or stop
	sProp.Scope = DICS_FLAG_CONFIGSPECIFIC;             // this device's own profile
	sProp.HwProfile = 0;                                // and no profile beyond it

	return SetupDiSetClassInstallParams(hDev, &sDev, &sProp.ClassInstallHeader, sizeof(sProp)) &&
		SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, hDev, &sDev);
}
//////////////////////////////////////////////////////////////////////////////////////////////////


///////////////////////////////////////////////////////////////////////////////////////////////////
// The ramdisk capability.
//
// What it does: the new size is written where the Ramdriv driver reads it when
// its device starts, and then the driver is made to start again -- the device is
// cycled, and the drive letter is polled until it is in the state the step being
// run is waiting for. The waits poll rather than sleep, because how long a
// device takes to come and go is not ours to know.
//
// The order is the contract: the size that is there now is read first, and a
// read that fails ends the command before anything is touched. That is why a
// machine with no Ramdriv service -- this PE, for one -- answers "no" instead of
// cycling a device class it knows nothing about.
//
// This is the one capability in this file whose working path the test rig cannot
// reach (see D-SLICES.md), so what follows is a faithful re-expression rather
// than a redesign: the only changes are the two places the reference could spin
// without an exit, and both are marked.
///////////////////////////////////////////////////////////////////////////////////////////////////
#define RAMDISK_KEY   TEXT("System\\CurrentControlSet\\Services\\Ramdriv\\Parameters")
#define RAMDISK_VALUE TEXT("DiskSize")
#define RAMDISK_DRIVE TEXT("%ramdrv%")

// The device to cycle. The class is enumerated to its end and the last device it
// produced is the one kept, which is what the reference's enumeration left
// behind. FALSE means there is no such device here.
static BOOL RamDiskDevice(HDEVINFO *phDev, SP_DEVINFO_DATA *pInfo)
{
	HDEVINFO hDev = INVALID_HANDLE_VALUE;
	SP_DEVINFO_DATA sInfo;
	BOOL bFound = FALSE;

	// The reference retried this call, but the counter it tested was never
	// decremented, so a class that never appears meant a spin with no way out.
	// RETRY attempts is what that counter was there for. (changed: bounded)
	for (UINT uTry = 0; (uTry < RETRY) && (hDev == INVALID_HANDLE_VALUE); uTry++)
	{
		hDev = SetupDiGetClassDevs(&Ramdrive, NULL, NULL, 0);
	}
	if (hDev == INVALID_HANDLE_VALUE)
	{
		return FALSE;
	}

	sInfo.cbSize = sizeof(sInfo);
	for (DWORD i = 0; SetupDiEnumDeviceInfo(hDev, i, &sInfo); i++)
	{
		*pInfo = sInfo;
		bFound = TRUE;
	}
	if (!bFound)
	{
		SetupDiDestroyDeviceInfoList(hDev);
		return FALSE;
	}
	*phDev = hDev;
	return TRUE;
}

// Cycle the device and wait for the drive letter to be there (bWant true) or to
// be gone (bWant false). FALSE when the wait ran out.
static BOOL RamDiskCycle(HDEVINFO hDev, SP_DEVINFO_DATA *pInfo, PCTSTR ptzDrive, DWORD dwFirst, DWORD dwSecond, BOOL bWant)
{
	for (UINT uTry = 0; uTry < RETRY; uTry++)
	{
		ManageDriver(hDev, *pInfo, dwFirst);
		ManageDriver(hDev, *pInfo, dwSecond);
		if (DiskIsExist(ptzDrive) == bWant)
		{
			return TRUE;
		}
	}
	return FALSE;
}

// Change the size the driver will use, and make it pick the new one up.
static BOOL SetRamdSize(DWORD dwNewSize)
{
	HDEVINFO hDev = INVALID_HANDLE_VALUE;
	SP_DEVINFO_DATA sInfo;
	TCHAR tzDrive[MAX_NAME];
	DWORD dwOldSize = 0;
	DWORD dwType = REG_DWORD;
	DWORD dwSize = sizeof(dwOldSize);
	BOOL bResult = FALSE;

	if (!RamDiskDevice(&hDev, &sInfo))
	{
		return FALSE;
	}

	// A size that cannot be read means this machine has no such driver to talk
	// to, so nothing is written and nothing is cycled.
	if (SHGetValue(HKEY_LOCAL_MACHINE, RAMDISK_KEY, RAMDISK_VALUE, &dwType, &dwOldSize, &dwSize) != ERROR_SUCCESS)
	{
		SetupDiDestroyDeviceInfoList(hDev);
		return FALSE;
	}
	if (dwNewSize == dwOldSize)
	{
		SetupDiDestroyDeviceInfoList(hDev);
		return TRUE;        // already that size - nothing to do
	}

	dwSize = sizeof(dwNewSize);
	if (SHSetValue(HKEY_LOCAL_MACHINE, RAMDISK_KEY, RAMDISK_VALUE, REG_DWORD, &dwNewSize, dwSize) != ERROR_SUCCESS)
	{
		SetupDiDestroyDeviceInfoList(hDev);
		return FALSE;
	}

	ExpandEnvironmentStrings(RAMDISK_DRIVE, tzDrive, _NumOf(tzDrive));
	if (UStrLen(tzDrive) < 2)
	{
		SetupDiDestroyDeviceInfoList(hDev);
		return FALSE;
	}

	// Down, then up again. The third wait is the reference's and is kept as it
	// stands: after waiting for the letter to arrive it waits for the letter to
	// be gone, and nothing in this repository records why. What it used to be
	// was an unbounded spin, so it is bounded now. (changed: bounded)
	if (RamDiskCycle(hDev, &sInfo, tzDrive, DICS_ENABLE, DICS_DISABLE, FALSE) &&
		RamDiskCycle(hDev, &sInfo, tzDrive, DICS_DISABLE, DICS_ENABLE, TRUE))
	{
		for (UINT uTry = 0; uTry < RETRY; uTry++)
		{
			if (!DiskIsExist(tzDrive))
			{
				bResult = TRUE;
				break;
			}
		}
	}
	if (!bResult)
	{
		// The value is already written, so the machine would come up with a
		// size nobody asked for. Put the old one back before answering no: the
		// driver in memory keeps whatever it read at load time either way, so
		// this only decides what the next boot does -- which is the part that
		// outlives the mistake.
		SHSetValue(HKEY_LOCAL_MACHINE, RAMDISK_KEY, RAMDISK_VALUE, REG_DWORD,
			&dwOldSize, sizeof(dwOldSize));
	}
	SetupDiDestroyDeviceInfoList(hDev);
	return bResult;
}

// The size comes through the same arithmetic FBWF uses -- a percentage of free
// memory kept inside the bounds -- with this command's own limits: 1 to 512 MB,
// and 32 MB when nothing is asked for.
//
// The answer is the drive letter, not the resize: a size that was already right
// succeeds without touching anything, and a machine with no such drive answers
// "no" either way. That is the reference's shape.
HRESULT SetRamDiskSizeMb(DWORD dwSizeMb)
{
	TCHAR tzDrive[MAX_NAME];
	UINT64 uBytes = (UINT64) dwSizeMb * 1024 * 1024;

	// The size is handed on as a 32-bit byte count, so a megabyte count that
	// does not fit in one is refused rather than wrapping: wrapped, a huge
	// request becomes a tiny ramdisk and still reports success.
	if (uBytes > 0xFFFFFFFFULL)
	{
		return HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER);
	}
	// The resize's own answer used to be dropped: what came back was only "is
	// there a ramdisk drive", so a write or a device cycle that failed reported
	// the same success as one that worked -- and the registry value had been
	// changed by then, so the next boot would use a size nobody asked for.
	if (!SetRamdSize((DWORD) uBytes))
	{
		return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
	}
	ExpandEnvironmentStrings(RAMDISK_DRIVE, tzDrive, _NumOf(tzDrive));
	return DiskIsExist(tzDrive) ? S_OK : S_FALSE;
}



////////////////////////////////////////////////////////////////////////////////////////////////////
