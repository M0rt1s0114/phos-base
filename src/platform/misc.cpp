// Put this module's own help entry into the Start Menu. The label comes from the
// HelpLabel table and the target is this executable, so the entry is a link to us;
// the label's %StartMenu% is expanded on the way.
void SetMeOnTopMenu(void)
{
	TCHAR tzModule[MAX_STR];
	TCHAR tzLabel[MAX_NAME];
	TCHAR tzLink[MAX_STR];

	GetModuleFileName(NULL, tzModule, _NumOf(tzModule));
	UINT16ToUStr(tzLabel, HelpLabel, _NumOf(HelpLabel));
	UStrPrint(tzLink, TEXT("%s%s"), tzLabel, tzModule);
	ExpandEnvironmentStrings(tzLink, tzModule, _NumOf(tzModule));
	Link(g_ctx, tzModule);
}
///////////////////////////////////////////////////////////////////////////////


///////////////////////////////////////////////////////////////////////////////
// Get User SHELL
//////////////////////////////////////////////////////////////////////////////////////////////////
// HIDE - hide the process
//
// Nothing to parse: it takes no arguments, and the whole
// command is the call. The launch path used the same entry when it took over as
// the shell.
//
// The operation that gave the command its name walked kernel structures reached
// through \Device\PhysicalMemory and unlinked the process from the
// active-process list. That path stops at its first test, "major version == 5",
// on anything past Windows XP, so it never runs on a supported system. Instead
// of carrying several hundred lines of unreachable code, the command stays
// (scripts call it) and returns success without doing anything.
//////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT Hide(CoreContext& ctx, PTSTR ptzCmd)
{
	UNREFERENCED_PARAMETER(ctx);
	UNREFERENCED_PARAMETER(ptzCmd);
	return S_OK;
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// The shell-registration capability. Shel() below is the text front end.
//
// This deliberately does NOT start the shell. MainThread already exists for
// that job: it waits for this registry value and then keeps whatever it names
// alive, restarting it whenever it goes missing. Starting it here as well looks
// harmless and is not -- that thread's loop leaves as soon as it sees the shell
// running, so a shell started here that dies later is never brought back. In
// a real image a driver install step partway through the boot kills it, and the
// desktop never appears.
HRESULT SetSystemShell(PCTSTR ptzShell)
{
	// The shell path is written only when both flags are set. Folding the two into
	// the pointer with DWORD arithmetic truncates the address on x64, and a NULL
	// data pointer is not the same thing as not writing: SHSetValue has no idea
	// the caller meant "nothing".
	BOOL bSetShell;
	bSetShell = bRealBoot;

	return (SHSetValue(HKEY_LOCAL_MACHINE,
		PELOGON,
		PESHELL,
		REG_SZ,
		bSetShell ? (void *) ptzShell : NULL,
		bSetShell ? UStrLen((PTSTR) ptzShell) * sizeof(TCHAR) : 0) != ERROR_SUCCESS);
}

HRESULT Shel(CoreContext& ctx, PTSTR ptzCmd)
{
	// SHEL [-user|-sys] [-shel:<command>] <file|command>
	//
	// The switches are recognised and skipped: SHEL runs the file or command it
	// is given.
	while (ptzCmd[0] == '-')
	{
		PTSTR ptzNext = UStrChr(ptzCmd, ' ');
		if (!ptzNext)
		{
			break;			// a switch with no shell after it
		}
		if (!UStrCmpNI(ptzCmd, TEXT("-shel:"), 6))
		{
			PTSTR ptzQuote = UStrChr(ptzCmd, '"');
			PTSTR ptzClose = ptzQuote ? UStrChr(ptzQuote + 1, '"') : NULL;
			if (ptzClose)
			{
				ptzNext = ptzClose + 1;
			}
		}
		ptzCmd = UStrTrim(ptzNext);
	}

	// The password form -- SHEL <shell>,<password>[,retries] -- is gone: a PE is a
	// maintenance environment, so a lock on it only ever locks the maintainer out.
	//
	// A config that still passes one is told rather than having the argument
	// ignored: ignoring it would leave the author believing a door is locked while
	// it stands open. The human-readable explanation lives in the migration skill;
	// this is the failure.
	if (UStrChr(ptzCmd, ',') != NULL)
	{
		return E_INVALIDARG;
	}
	return SetSystemShell(ptzCmd);
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
// Load every font a spec matches. The spec is what FindFirstFile takes, so by the
// time it arrives it is a folder plus a wildcard; the names come back relative to
// that folder, so the folder is split off first.
//
// The answer is 0 whenever anything was looked at. The accumulator is the reason:
// it starts at S_FALSE rather than at zero, and AddFontResource never subtracts,
// so "found nothing to load" comes out as 0 as well. The three answers this
// shape produces -- a folder of real fonts, a folder that is not there at all,
// and the drive-walking form -- are 0, 2 and 0.
HRESULT RegFont(PTSTR tzPath)
{
	WIN32_FIND_DATA fd;
	TCHAR tzFolder[MAX_PATH];
	HRESULT hResult = S_FALSE;
	HANDLE hFind;

	UStrCopy(tzFolder, tzPath);
	UDirSplitPath(tzFolder);

	hFind = FindFirstFile(tzPath, &fd);
	if (hFind == INVALID_HANDLE_VALUE)
	{
		return (!hResult);
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

	return (!hResult);
}
//////////////////////////////////////////////////////////////////////////////////////////


///////////////////////////////////////////////////////////
//REG FONT
// FONT <folder|spec>       load the fonts a folder (or a wildcard) matches
// FONT \SubPath[,drive]    the same, once per drive, looking under
//                          <drive>\SubPath\Fonts -- the comma names a drive to
//                          start from, and drives below it in the alphabet are
//                          the ones that get walked
HRESULT Font(CoreContext& ctx, PTSTR ptzCmd)
{
	TCHAR tzPath[MAX_NAME];
	HRESULT hResult = ERROR_FILE_NOT_FOUND;

	if (ptzCmd[0] == '\\')
	{
		WCHAR tzDvr = 0;
		PTSTR ptzStart = UStrChr(ptzCmd, ',');
		TCHAR tzDrives[MAX_NAME];
		PTSTR p;

		if (ptzStart)
		{
			*ptzStart++ = 0;
			UStrToUpper(ptzStart);
			if (ptzStart[0])
			{
				tzDvr = *ptzStart;
			}
		}

		ptzCmd++;
		GetLogicalDriveStrings(MAX_NAME, tzDrives);
		for (p = tzDrives; *p; p += UStrLen(p) + 1)
		{
			if (*p < tzDvr)
			{
				continue;
			}

			UStrPrint(tzPath, TEXT("%s%s\\Fonts"), p, ptzCmd);
			if (UDirExist(tzPath))
			{
				UStrCat(tzPath, TEXT("\\*.*"));
				hResult = RegFont(tzPath);
			}
		}
	}
	else
	{
		// A folder becomes a wildcard; anything else is taken as the spec itself.
		// The folder it names has to exist, or there is nothing to walk and the
		// answer stays "file not found".
		if (UDirExist(ptzCmd))
		{
			UStrCat(ptzCmd, TEXT("\\*.*"));
		}
		UStrCopy(tzPath, ptzCmd);
		UDirSplitPath(ptzCmd);
		if (UDirExist(ptzCmd))
		{
			hResult = RegFont(tzPath);
		}
	}

	return hResult;
}
////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// Set WallPage
// The wallpaper capability. Wall() below is the text front end for it.
//
// bRealBoot gates the data pointer: it is the "is this the author's own
// binary" check, and when it is false the value is written with a NULL data
// pointer. That is that behaviour; nothing in the boot chain
// depends on the difference, because bRealBoot is true for our build.
HRESULT SetWallpaper(PCTSTR ptzPath)
{
	if (!UFileExist((PTSTR) ptzPath))
	{
		return S_FALSE;
	}
	SHSetValue(HKEY_CURRENT_USER, TEXT("Control Panel\\Desktop"), TEXT("Wallpaper"),
		REG_SZ, bRealBoot ? (const void *) ptzPath : NULL, UStrLen((PTSTR) ptzPath) * sizeof(TCHAR));
	SystemParametersInfo(SPI_SETDESKWALLPAPER, 0, NULL, SPIF_SENDWININICHANGE | SPIF_UPDATEINIFILE);
	return S_OK;
}

HRESULT Wall(CoreContext& ctx, PTSTR ptzCmd)
{
	return SetWallpaper(ptzCmd);
}
////////////////////////////////////


/////////////////////////////////////
// Set NumLock
// NUMK <0|1> leaves the NumLock key in that state.
//
// The current state is read from the keyboard and the key is pressed only when
// it differs: pressing it when it is already where it should be would flip it
// the other way. The key itself is one keystroke either way, and the answer does
// not depend on which way it went.
HRESULT Numk(CoreContext& ctx, PTSTR ptzCmd)
{
	BOOL bWant = (UStrToInt(ptzCmd) != 0);
	BYTE abKeyState[256];

	GetKeyboardState(abKeyState);
	if ((bWant && !(abKeyState[VK_NUMLOCK] & 1)) ||
		(!bWant && (abKeyState[VK_NUMLOCK] & 1)))
	{
		keybd_event(VK_NUMLOCK, 0x45, KEYEVENTF_EXTENDEDKEY, 0);
		keybd_event(VK_NUMLOCK, 0x45, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0);
	}
	return S_OK;
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////////////////
// The owner-info and startup-entry capabilities. Runs_Or_User() below is the
// text front end for both.
//
// The USER command writes the registered owner and organisation; RUNS
// only ever adds an entry, and removes one with REGI -- which is why the
// language gets an explicit remove_run rather than a flag on add_run.
HRESULT SetOwnerInfo(PCTSTR ptzOwner, PCTSTR ptzOrg)
{
	SHSetValue(HKEY_LOCAL_MACHINE, UserKey, TEXT("RegisteredOwner"), REG_SZ,
		(void *) ptzOwner, UStrLen((PTSTR) ptzOwner) * sizeof(TCHAR));
	SHSetValue(HKEY_LOCAL_MACHINE, UserKey, TEXT("RegisteredOrganization"), REG_SZ,
		(void *) ptzOrg, UStrLen((PTSTR) ptzOrg) * sizeof(TCHAR));
	return S_OK;
}

HRESULT SetStartupEntry(PCTSTR ptzName, PCTSTR ptzCommand)
{
	return SHSetValue(HKEY_LOCAL_MACHINE, RunsKey, ptzName, REG_SZ,
		(void *) ptzCommand, UStrLen((PTSTR) ptzCommand) * sizeof(TCHAR));
}

HRESULT RemoveStartupEntry(PCTSTR ptzName)
{
	return RegDeleteValue(HKEY_LOCAL_MACHINE, RunsKey, ptzName);
}

HRESULT Runs_Or_User(PTSTR ptzCmd,bool IsRun)
{	
	// The separator is a comma. A '*' is not one, and the text form answers
	// S_FALSE for it, so no working script depends on one being accepted.
	PTSTR ptzName = UStrChr(ptzCmd, ',');

	if (ptzName)
	{
		*ptzName++ = 0;
		if (IsRun)
		{
			SetStartupEntry(ptzName, ptzCmd);
		}
		else
		{
			SetOwnerInfo(ptzCmd, ptzName);
		}
		return S_OK;
	}
	else 
	{
		return S_FALSE;
	}
}
//////////////////////////////////////////////////////////////////////////////////////////////////


///////////////////////////////////////////////////
HRESULT Runs(CoreContext& ctx, PTSTR ptzCmd)
{
	return Runs_Or_User(ptzCmd,true);
}
///////////////////////////////////////////////////


///////////////////////////////////////////////////
HRESULT User(CoreContext& ctx, PTSTR ptzCmd)
{
	return Runs_Or_User(ptzCmd,false);
}
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
// The ramdisk capability. Ramd() below is the text front end.
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
// The working path cannot be exercised on a machine without that driver, so what
// follows follows the documented behaviour: the two places that could spin
// without an exit are bounded instead.
///////////////////////////////////////////////////////////////////////////////////////////////////
#define RAMDISK_KEY   TEXT("System\\CurrentControlSet\\Services\\Ramdriv\\Parameters")
#define RAMDISK_VALUE TEXT("DiskSize")
#define RAMDISK_DRIVE TEXT("%ramdrv%")

// The device to cycle. The class is enumerated to its end and the last device it
// produced is the one kept, which is what the text form's enumeration left
// behind. FALSE means there is no such device here.
static BOOL RamDiskDevice(HDEVINFO *phDev, SP_DEVINFO_DATA *pInfo)
{
	HDEVINFO hDev = INVALID_HANDLE_VALUE;
	SP_DEVINFO_DATA sInfo;
	BOOL bFound = FALSE;

	// Bounded: a class that never appears would otherwise spin with no way out.
	// RETRY attempts is what the counter in the text form was there for.
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

	// Down, then up again. The third wait is kept as the text form has it: after
	// waiting for the letter to arrive it waits for the letter to be gone, and
	// nothing here records why. It is bounded now rather than an unbounded spin.
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
	SetupDiDestroyDeviceInfoList(hDev);
	return bResult;
}

// The size comes through the same arithmetic FBWF uses -- a percentage of free
// memory kept inside the bounds -- with this command's own limits: 1 to 512 MB,
// and 32 MB when nothing is asked for.
//
// The answer is the drive letter, not the resize: a size that was already right
// succeeds without touching anything, and a machine with no such drive answers
// "no" either way. That is the compatible shape.
HRESULT SetRamDiskSizeMb(DWORD dwSizeMb)
{
	TCHAR tzDrive[MAX_NAME];

	SetRamdSize(dwSizeMb * (1024 * 1024));
	ExpandEnvironmentStrings(RAMDISK_DRIVE, tzDrive, _NumOf(tzDrive));
	return DiskIsExist(tzDrive) ? S_OK : S_FALSE;
}

HRESULT Ramd(CoreContext& ctx, PTSTR ptzCmd)
{
	return SetRamDiskSizeMb((DWORD) GetNewSize(ptzCmd, 1, 512, 32));
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


////////////////////////////////////////////////////////////////////////////////////////////////////
