// Turn one privilege on for this process. The callers need it for a single
// operation -- creating a page file, shutting the machine down -- and none of
// them reads the answer, which is just as well: what comes back is the Win32
// BOOL rather than an HRESULT, so "it worked" is 1 and "the token would not
// open" is 0.
//
// The token is closed on both paths. It used to be left open, which is one
// leaked handle per call, and these are called on the boot path.
HRESULT Priv(PCTSTR ptzName)
{
	HANDLE hToken = NULL;
	TOKEN_PRIVILEGES tPriv;
	BOOL bResult = FALSE;

	if (!OpenProcessToken(CurrentProcess, TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
	{
		return FALSE;
	}

	UMemSet(&tPriv, 0, sizeof(tPriv));
	tPriv.PrivilegeCount = 1;
	tPriv.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	if (LookupPrivilegeValue(NULL, ptzName, &tPriv.Privileges[0].Luid))
	{
		// AdjustTokenPrivileges answers TRUE when it changed nothing, because
		// the token does not hold the privilege at all -- the caller has to ask
		// for the reason. Without this a process without SeShutdownPrivilege
		// believed it had switched one on. The last error has to be cleared
		// first: a successful call does not clear it, so a stale
		// ERROR_NOT_ALL_ASSIGNED from something else would read as a failure.
		SetLastError(ERROR_SUCCESS);
		bResult = AdjustTokenPrivileges(hToken, FALSE, &tPriv, sizeof(TOKEN_PRIVILEGES), NULL, NULL);
		if (bResult && (GetLastError() == ERROR_NOT_ALL_ASSIGNED))
		{
			bResult = FALSE;
		}
	}
	CloseHandle(hToken);

	return bResult ? S_OK : S_FALSE;
}
///////////////////////////////////////////////////////////////////////////////////////////////////


///////////////////////////////////////////////////////////////////////////////////////////////////
// Put one mode in place. The answer is the DISP_CHANGE_* code the display driver
// gave back -- 0 when it took, 0xFFFFFFFE (DISP_CHANGE_BADMODE) for a mode no
// adapter has. The reference hands that code straight back: the same call
// answers 0 for a mode that exists and 0xFFFFFFFE for one that does not, so it
// is not a normalised success/failure.
//
// dmNew is left describing what is on screen now; on a failure the old mode goes
// back first, so that is what it describes.
HRESULT SetNewDisplay(DEVMODE &dmOld,DEVMODE &dmNew)
{
	// Ask first, apply second. CDS_UPDATEREGISTRY is the flag that also stores
	// the mode, and a mode the driver refuses must not end up in the registry
	// to come back at the next boot -- the rollback below only puts the *screen*
	// back, it does not unsay what was written. CDS_TEST answers with the same
	// DISP_CHANGE_* codes, so the caller still sees the driver's own answer --
	// 0xFFFFFFFE for a mode no adapter has.
	LONG lResult = ChangeDisplaySettings(&dmNew, CDS_TEST);

	if (lResult == DISP_CHANGE_SUCCESSFUL)
	{
		lResult = ChangeDisplaySettings(&dmNew, CDS_UPDATEREGISTRY);
	}

	if (lResult == DISP_CHANGE_SUCCESSFUL)
	{
		EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &dmOld);
	}
	else
	{
		ChangeDisplaySettings(&dmOld, CDS_UPDATEREGISTRY);
	}
	dmNew = dmOld;
	return (HRESULT) lResult;
}
///////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// Set one mode by its four figures. The tray menu is the caller: its items name a
// width, a height, a colour depth and a refresh rate, and nothing else on the
// boot path asks for a mode change.
//
// The fields go in one at a time, in the order the old menu applied them, and the
// answers add up -- a mode a driver will not take comes back as DISP_CHANGE_BADMODE
// in the total rather than being smoothed into a plain failure. A figure of zero
// means "leave this one alone".
//
// One at a time rather than one call with four fields, because that is what the
// menu has always done and some drivers only accept a size and a depth that are
// changed in separate calls.
HRESULT ApplyDisplayMode(DWORD uWidth, DWORD uHeight, DWORD uBits, DWORD uHz)
{
	DEVMODE dmOld;
	DEVMODE dmNew;
	HRESULT lResult = S_OK;

	// The current mode, or there is nothing to base a partial change on: the
	// figures the caller does not name come from here, and reading them from an
	// uninitialised stack frame is what /analyze complains about.
	ZeroMemory(&dmOld, sizeof(dmOld));
	if (!EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &dmOld))
	{
		return (HRESULT) GetLastError();
	}

	if (uWidth && uHeight &&
		((dmOld.dmPelsWidth != uWidth) || (dmOld.dmPelsHeight != uHeight)))
	{
		dmNew = dmOld;
		dmNew.dmPelsWidth = uWidth;
		dmNew.dmPelsHeight = uHeight;
		lResult += SetNewDisplay(dmOld, dmNew);
	}
	if (uBits && (dmOld.dmBitsPerPel != uBits))
	{
		dmNew = dmOld;
		dmNew.dmBitsPerPel = uBits;
		lResult += SetNewDisplay(dmOld, dmNew);
	}
	if (uHz && (dmOld.dmDisplayFrequency != uHz))
	{
		dmNew = dmOld;
		dmNew.dmDisplayFrequency = uHz;
		lResult += SetNewDisplay(dmOld, dmNew);
	}
	return lResult;
}
//////////////////////////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Pick a volume to put a page file on, out of the ones that have room for it.
//
// The caller names the drive it would rather use. The two answers come back
// through one array rather than a struct: slot 0 is the last removable volume
// with room, slot 1 the last fixed one, and the return value says whether a
// fixed volume at or after the preferred letter was found. The page-file code
// below indexes that array with this answer, which is why the slots sit where
// they do.
// The drive a page file may go on: the first partition at or after StartDrive with
// room for it. A removable drive with room, if one turns up first, is remembered
// in ptzDrive[0]; a fixed one goes in ptzDrive[1] -- so the caller can tell a
// "this one" answer from a "some fixed drive" one.
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// Set page file
#include <NTSecAPI.h>
#define REG_PageFile TEXT("PagingFiles")
HRESULT CreatePageFileAt(PCTSTR ptzPath, DWORD uMin, DWORD uMax);
// PAGE <path> [<min mb>] [<max mb>]
//
// A page file is only worth making when memory is short, so this does nothing --
// and answers S_OK -- when there is more free memory than the maximum asked for.
//
// When it does act, the volume in the path is only a preference: the drive
// search picks a fixed volume with room, falling back to a removable one, and
// then the file is created and recorded in one step, because a page file Windows
// does not know about is not one it will use.

// The page-file capability.
//
// It makes the file through ntdll and records it where Windows looks for page
// files, both in one step: a page file nobody recorded is not one the system
// will use.
// Make the page file and record it, in that order: NtCreatePagingFile builds the
// file, and the PagingFiles value is what makes Windows look for it at startup.
//
// The path is a DOS path -- the device name is looked up because that is what
// the call wants -- and the value is a list, so an entry already in it is
// appended to rather than replaced.
HRESULT CreatePageFileAt(PCTSTR ptzPath, DWORD uMin, DWORD uMax)
{
	TCHAR tzDrive[4];
	TCHAR tzDos[MAX_PATH];
	TCHAR tzFile[MAX_PATH];
	WCHAR wzPath[MAX_PATH];
	UNICODE_STRING sPath;
	ULARGE_INTEGER ulMin, ulMax;
	const DWORD cbPerMb = 1024 * 1024;
	HRESULT hResult;
	typedef NTSTATUS (NTAPI* PNtCreatePagingFile)(PUNICODE_STRING sPath, PULARGE_INTEGER puInitSize, PULARGE_INTEGER puMaxSize, ULONG uPriority);
	PNtCreatePagingFile pfn;

	if (UStrLen((PTSTR) ptzPath) < 3)
	{
		return E_INVALIDARG;
	}
	UStrCopyN(tzDrive, ptzPath, 3);          // "X:" and its terminator
	if (!UStrCopyTo(tzFile, (PTSTR) ptzPath + 2))
	{
		return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
	}

	// The call wants the device name, and a script has a drive letter.
	if (!QueryDosDevice(tzDrive, tzDos, MAX_PATH))
	{
		return (HRESULT) GetLastError();
	}
	// The device name and the path inside it. Both halves are bounded on their
	// own; their sum is not, and the file name comes from the script -- so the
	// combined length is checked rather than copied into the buffer.
	if (!UStrCatTo(tzDos, tzFile))
	{
		return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
	}
	UStrToWStr(wzPath, tzDos, MAX_PATH);

	sPath.Length = (USHORT) (UWStrLen(wzPath) * sizeof(WCHAR));
	sPath.MaximumLength = sPath.Length + sizeof(WCHAR);
	sPath.Buffer = wzPath;

	ulMin.QuadPart = uMin * cbPerMb;
	ulMax.QuadPart = uMax * cbPerMb;

	{
		HMODULE hNtdll = GetModuleHandle(TEXT("NTDLL"));

		pfn = hNtdll ? (PNtCreatePagingFile) GetProcAddress(hNtdll, "NtCreatePagingFile") : NULL;
	}
	if (!pfn)
	{
		return E_FAIL;
	}

	Priv(SE_CREATE_PAGEFILE_NAME);
	hResult = pfn(&sPath, &ulMin, &ulMax, 0);
	if (hResult == S_OK)
	{
		// Append to the list Windows reads. It is NUL separated, so the length
		// that comes back already counts the last terminator and the new entry
		// goes where that one is; a second terminator closes the pair.
		TCHAR tzStr[MAX_PATH];
		DWORD i = sizeof(tzStr);

		if (SHGetValue(HKEY_LOCAL_MACHINE, REG_MemMgr, REG_PageFile, NULL, tzStr, &i) != S_OK)
		{
			i = 0;
		}
		else
		{
			i = (i / sizeof(TCHAR)) - 1;
		}
		if ((i >= 0) && (i < (INT) _NumOf(tzStr) - 1))
		{
			UStrPrintN(tzStr + i, (size_t) (_NumOf(tzStr) - (size_t) i),
				TEXT("%s %d %d"), ptzPath, (int) uMin, (int) uMax);
			i += (INT) UStrLen(tzStr + i);
			tzStr[++i] = 0;		// the second terminator of the pair
		}
		// i is the index of that second terminator, so the byte count is i + 1
		// characters: writing i of them stored a MULTI_SZ with a single NUL --
		// the class of bug F-01 fixed for REG_SZ, in the other string type.
		{
			LONG lResult = SHSetValue(HKEY_LOCAL_MACHINE, REG_MemMgr, REG_PageFile,
				REG_MULTI_SZ, tzStr, (i + 1) * sizeof(TCHAR));

			if (lResult != ERROR_SUCCESS)
			{
				// The file was created but Windows has not been told about it,
				// which is not the thing that was asked for. SHSetValue hands
				// back the error code itself, so that is what goes up the stack.
				return (HRESULT) lResult;
			}
		}
	}
	return hResult;
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// Wait for a service to settle into the state we asked for. The service
// manager reports START_PENDING / STOP_PENDING while it works, so poll until
// that clears. The cap keeps a service that never finishes from stalling the
// boot forever; a start that fails outright is reported by the call itself,
// so this loop is only reached when the service manager accepted the request.
static BOOL ServWaitForState(SC_HANDLE hService, DWORD dwWanted)
{
	const DWORD dwCapMs = 30000;
	const DWORD dwStepMs = 100;
	DWORD dwWaited = 0;

	for (;;)
	{
		SERVICE_STATUS sStatus = {0};
		if (!QueryServiceStatus(hService, &sStatus))
		{
			return FALSE;
		}
		if (sStatus.dwCurrentState == dwWanted)
		{
			return TRUE;
		}
		if ((sStatus.dwCurrentState != SERVICE_START_PENDING) &&
			(sStatus.dwCurrentState != SERVICE_STOP_PENDING))
		{
			return FALSE;	// settled somewhere else (e.g. it failed to start)
		}
		if (dwWaited >= dwCapMs)
		{
			return FALSE;
		}
		Sleep(dwStepMs);
		dwWaited += dwStepMs;
	}
}
//////////////////////////////////////////////////////////////////////////////////////////////////
// The service capability.
//
// bWait is the part that matters: "net start", which the boot chain used to
// run, returns once the service is RUNNING. Starting without waiting is a
// different operation, and the chain has to ask for the wait explicitly.
// Reading a service's status, creating an entry, and removing one. Everything
// a caller can act on is in the SERVICE_STATUS the service manager hands back;
// the rest is the service manager's own bookkeeping.
HRESULT QueryServiceState(PCTSTR ptzName, SERVICE_STATUS *pStatus)
{
	SC_HANDLE hManager = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
	HRESULT hr;

	if (!hManager)
	{
		return (HRESULT) GetLastError();
	}
	{
		SC_HANDLE hService = OpenService(hManager, (PTSTR) ptzName, SERVICE_QUERY_STATUS);

		if (!hService)
		{
			hr = (HRESULT) GetLastError();
		}
		else
		{
			hr = QueryServiceStatus(hService, pStatus) ? S_OK : (HRESULT) GetLastError();
			CloseServiceHandle(hService);
		}
		CloseServiceHandle(hManager);
	}
	return hr;
}

HRESULT CreateServiceEntry(PCTSTR ptzName, PCTSTR ptzPath, PCTSTR ptzDisplay,
	DWORD dwType, DWORD dwStart, DWORD dwError, PCTSTR ptzDeps)
{
	SC_HANDLE hManager = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
	HRESULT hr;

	if (!hManager)
	{
		return (HRESULT) GetLastError();
	}
	{
		SC_HANDLE hService = CreateService(hManager, (PTSTR) ptzName,
			(ptzDisplay != NULL) ? ptzDisplay : ptzName, SERVICE_ALL_ACCESS,
			dwType, dwStart, dwError, ptzPath, NULL, NULL, ptzDeps, NULL, NULL);

		if (!hService)
		{
			hr = (HRESULT) GetLastError();
		}
		else
		{
			CloseServiceHandle(hService);
			hr = S_OK;
		}
		CloseServiceHandle(hManager);
	}
	return hr;
}

HRESULT DeleteServiceEntry(PCTSTR ptzName, BOOL bStopFirst)
{
	SC_HANDLE hManager = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
	HRESULT hr;

	if (!hManager)
	{
		return (HRESULT) GetLastError();
	}
	{
		// DELETE to remove it, STOP because stopping first is what the
		// reference's "-stop-" form does and the service manager refuses to
		// delete a running one otherwise.
		SC_HANDLE hService = OpenService(hManager, (PTSTR) ptzName,
			DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS);

		if (!hService)
		{
			hr = (HRESULT) GetLastError();
		}
		else
		{
			if (bStopFirst)
			{
				SERVICE_STATUS sStatus;

				ControlService(hService, SERVICE_CONTROL_STOP, &sStatus);
			}
			hr = DeleteService(hService) ? S_OK : (HRESULT) GetLastError();
			CloseServiceHandle(hService);
		}
		CloseServiceHandle(hManager);
	}
	return hr;
}

HRESULT SetServiceState(PCTSTR ptzName, BOOL bStop, BOOL bWait)
{
	SC_HANDLE hManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);

	if (!hManager)
	{
		return (HRESULT) GetLastError();
	}
	SC_HANDLE hService = OpenService(hManager, (PTSTR) ptzName,
		SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);

	if (!hService)
	{
		// A service that is not there answers 1060, which is what the reference
		// reports. Answering S_FALSE here was a long standing difference that
		// nothing noticed, because nothing exercised SERV at all.
		HRESULT hr = (HRESULT) GetLastError();
		CloseServiceHandle(hManager);
		return hr;
	}
	{
		BOOL bCalled;
		HRESULT hr = S_OK;

		if (bStop)
		{
			SERVICE_STATUS sStatus;
			bCalled = ControlService(hService, SERVICE_CONTROL_STOP, &sStatus);
		}
		else
		{
			bCalled = StartService(hService, 0, NULL);
		}

		if (!bCalled)
		{
			hr = (HRESULT) GetLastError();
		}
		else if (bWait && !ServWaitForState(hService, bStop ? SERVICE_STOPPED : SERVICE_RUNNING))
		{
			// The request was accepted but the service did not settle where it
			// was asked to. The service manager's own code for "did not answer
			// in time" is the honest answer; the reference documents no value
			// for -wait at all.
			hr = (HRESULT) ERROR_SERVICE_REQUEST_TIMEOUT;
		}
		CloseServiceHandle(hService);
		CloseServiceHandle(hManager);
		return hr;
	}
}
//////////////////////////////////////////////////////////////////////////////////////////////////
// Start / stop a service or driver
//
// Reference syntax: SERV [-wait] [start switch] [?result var] [!]<service name>
//
// There is no service here before the call, so the "query" half of the
// reference syntax is not implemented. The start-type switches only mean
// something for SERV-create, which is not implemented either, so they are
// skipped rather than rejected.
//////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
// The FBWF cache size, from values rather than from the "P20 L32 H64" text:
// a percentage of free memory, kept between the minimum and the maximum. A
// percentage of 0 means "no percentage term at all" -- the caller maps the
// legacy "P0" onto 1 before it gets here, because that is what the reference
// does with a percentage below one.
//
// Every way in ends up here, so the arithmetic that decides how much
// write-filter cache a machine gets exists once.
// 0 at either end means the caller did not ask for that end, so it is passed
// through instead of being pulled up to MIN.
static DWORD ClampToRange(DWORD dwValue, DWORD MIN, DWORD MAX)
{
	if (dwValue == 0)
	{
		return 0;
	}
	if (dwValue < MIN)
	{
		return MIN;
	}
	if (dwValue > MAX)
	{
		return MAX;
	}
	return dwValue;
}
// The band the write-filter cache may live in, in megabytes, and what it gets
// when the caller names neither end. All three were spelled MD5Str_Len, 1024
// and 32 -- a digest length standing in for a size in megabytes.
const DWORD c_fbwfMinMb = 16;
const DWORD c_fbwfMaxMb = 1024;
const DWORD c_fbwfDefaultMb = 32;

DWORD FbwfCacheSize(DWORD part_mem, DWORD min_mem, DWORD max_mem,
	DWORD MIN, DWORD MAX, DWORD DefSize)
{
	DWORD dwPercent = part_mem;
	DWORD dwMin = min_mem;
	DWORD dwMax = max_mem;
	DWORD dwSize = 0;

	// The percentage term, capped at half of what is free. A percentage of 0
	// means "no percentage term at all", which is why the size can still be 0
	// when this is done.
	if (dwPercent > 0)
	{
		if (dwPercent > 50)
		{
			dwPercent = 50;
		}
		dwSize = (DWORD) (dwPercent * GetFreeMemo() / 100); // P% of the free memory
	}

	// Both ends are brought inside the range this cache may have, and a pair
	// that arrived the wrong way round is swapped rather than rejected.
	dwMin = ClampToRange(dwMin, MIN, MAX);
	dwMax = ClampToRange(dwMax, MIN, MAX);
	if (dwMax < dwMin) // swap the two
	{
		DWORD dwSwap = dwMin;

		dwMin = dwMax;
		dwMax = dwSwap;
	}

	// Then the answer is pulled into the band -- a size that never got computed
	// counts as "too small" here -- and a size that is still 0 falls back to the
	// default the caller named.
	if ((dwMax > 0) && ((dwSize > dwMax) || (dwSize == 0)))
	{
		dwSize = dwMax;
	}
	if ((dwMin > 0) && (dwSize < dwMin))
	{
		dwSize = dwMin;
	}
	if (dwSize == 0)
	{
		dwSize = DefSize;
	}

	return dwSize;
}

// The write-filter capability both front ends end at: record the size the
// service will read, then ask it to start. The value is what the service reads
// when it comes up, which is why the two steps belong to one operation.
#define FBWFKEY TEXT("SYSTEM\\ControlSet001\\Services\\FBWF")
#define FBWFVALUE TEXT("WinPECacheThreshold")
#define FBWFSERVICE TEXT("FBWF")
HRESULT ApplyFbwfCache(DWORD dwSize)
{
	// The value is what the service reads when it comes up, so a write that did
	// not land means the service starts on the old threshold -- reporting the
	// call as done would be saying "the cache is now this size" about a size
	// nobody will use.
	{
		LONG lResult = SHSetValue(HKEY_LOCAL_MACHINE, FBWFKEY, FBWFVALUE, REG_DWORD,
			&dwSize, sizeof(dwSize));

		if (lResult != ERROR_SUCCESS)
		{
			return (HRESULT) lResult;
		}
	}
	return SetServiceState(FBWFSERVICE, FALSE, FALSE);
}
//////////////////////////////////////////////////////////////////////////////////////



//////////////////////////////////////////////////////////////////////////////////////////////////
// Test string equal
template <typename T1, typename T2> inline BOOL TStrEqual(T1 ptStr1, T2 ptStr2, UINT uLen)
{
	for (UINT i = 0; i < uLen; i++)
	{
		if (!UChrEqual((TCHAR)ptStr1[i], (TCHAR)ptStr2[i]))
		{
			return FALSE;
		}
	}
	return TRUE;
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Search string
template <typename T1, typename T2> PVOID TStrStr(T1 ptStr1, T2 ptStr2, UINT uLen)
{
	for (T1 p = ptStr1; *p; p++)
	{
		if (TStrEqual(p, ptStr2, uLen))
		{
			return p;
		}
	}
	return NULL;
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Lookup device ID from INFs
