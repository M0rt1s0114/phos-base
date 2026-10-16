//////////////////////////////////////////////////////////////////////////////////////////////////
// The registry capabilities
//
// These are the operations themselves: a root key, a subkey, a value name and
// typed data. Regi() below is the legacy text front end for the same
// operations, and the language's reg.* verbs call these directly -- before
// this, the language built "REGI HKLM\...\name=data" strings and handed them
// back to the parser it is supposed to stop depending on.
//////////////////////////////////////////////////////////////////////////////////////////////////
HKEY RegRootFromName(PCTSTR ptzName)
{
	struct { PCTSTR ptzName; HKEY hKey; } const c_roots[] =
	{
		{ TEXT("HKLM"),                HKEY_LOCAL_MACHINE },
		{ TEXT("HKEY_LOCAL_MACHINE"),  HKEY_LOCAL_MACHINE },
		{ TEXT("HKCU"),                HKEY_CURRENT_USER },
		{ TEXT("HKEY_CURRENT_USER"),   HKEY_CURRENT_USER },
		{ TEXT("HKCR"),                HKEY_CLASSES_ROOT },
		{ TEXT("HKEY_CLASSES_ROOT"),   HKEY_CLASSES_ROOT },
		{ TEXT("HKU"),                 HKEY_USERS },
		{ TEXT("HKEY_USERS"),          HKEY_USERS },
		{ TEXT("HKCC"),                HKEY_CURRENT_CONFIG },
		{ TEXT("HKEY_CURRENT_CONFIG"), HKEY_CURRENT_CONFIG },
	};

	for (UINT i = 0; i < _NumOf(c_roots); i++)
	{
		if (UStrCmpI(ptzName, c_roots[i].ptzName) == 0)
		{
			return c_roots[i].hKey;
		}
	}
	return NULL;
}

HRESULT RegSetValue(HKEY hRoot, PCTSTR ptzSubKey, PCTSTR ptzName, DWORD dwType,
	const void *pvData, DWORD cbData)
{
	return SHSetValue(hRoot, ptzSubKey, ptzName, dwType, pvData, cbData);
}

// Deleting something that is not there leaves the caller in the state it asked
// for, so "not found" counts as success here. Every other result passes
// through unchanged.
HRESULT RegDeleteValue(HKEY hRoot, PCTSTR ptzSubKey, PCTSTR ptzName)
{
	LONG lResult = SHDeleteValue(hRoot, ptzSubKey, ptzName);

	if ((lResult == ERROR_SUCCESS) || (lResult == ERROR_FILE_NOT_FOUND) ||
		(lResult == ERROR_PATH_NOT_FOUND))
	{
		return S_OK;
	}
	return (HRESULT) lResult;
}

// Not the SDK's RegDeleteTree. That one appeared in Vista and is declared now
// that _WIN32_WINNT says Windows 7, so this cannot keep the name; what it adds
// is the guard below and the "already gone is fine" rule.
// The mount points RegLoadHive created. A delete that reaches one of these
// writes through to a real installation's hive on disk, and a reboot does not
// put it back -- so the guard below wants the caller to have said `force`.
// A failed load is not recorded and an unload forgets.
static const UINT c_maxLoadedHives = 16;
static TCHAR s_loadedHives[c_maxLoadedHives][MAX_NAME];
static UINT  s_loadedHiveCount = 0;

static VOID RememberLoadedHive(PCTSTR ptzMount)
{
	if ((s_loadedHiveCount < c_maxLoadedHives) && UStrCopyTo(s_loadedHives[s_loadedHiveCount], ptzMount))
	{
		s_loadedHiveCount++;
	}
}

static VOID ForgetLoadedHive(PCTSTR ptzMount)
{
	for (UINT i = 0; i < s_loadedHiveCount; i++)
	{
		if (!UStrCmp(s_loadedHives[i], ptzMount))
		{
			s_loadedHiveCount--;
			UStrCopyTo(s_loadedHives[i], s_loadedHives[s_loadedHiveCount]);
			s_loadedHives[s_loadedHiveCount][0] = 0;
			return;
		}
	}
}

// Is this subkey the mount point of a loaded hive, or something under one?
BOOL RegIsLoadedHive(PCTSTR ptzSubKey)
{
	UINT uLen = UStrLen(ptzSubKey);

	for (UINT i = 0; i < s_loadedHiveCount; i++)
	{
		UINT uName = UStrLen(s_loadedHives[i]);

		if ((uName != 0) && (uLen >= uName) &&
			(UStrCmpNI(s_loadedHives[i], ptzSubKey, uName) == 0) &&
			((ptzSubKey[uName] == 0) || (ptzSubKey[uName] == '\\')))
		{
			return TRUE;
		}
	}
	return FALSE;
}

HRESULT RegDeleteTreeGuarded(HKEY hRoot, PCTSTR ptzSubKey, BOOL bForce)
{
	LONG lResult;

	// An empty subkey is the root itself: SHDeleteKey(HKLM, "") walks
	// everything underneath HKLM. The subkey arrives from a script, and a
	// script that ends up with an empty one is a script that expanded a
	// variable to nothing -- "REGI HKLM\%&k%\" with k unset.
	if (!ptzSubKey || !ptzSubKey[0])
	{
		return E_INVALIDARG;
	}

	// A key directly under a root is one call away from taking a whole branch
	// with it: SHDeleteKey(HKLM, "SOFTWARE") is all of SOFTWARE. Two levels
	// are the price of a delete that a script can be sure it meant.
	if (!UStrChr(ptzSubKey, '\\'))
	{
		return E_INVALIDARG;
	}

	// And a hive reg.load_hive attached goes further than this machine: the
	// write lands in somebody's real installation.
	if (RegIsLoadedHive(ptzSubKey) && !bForce)
	{
		return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
	}

	lResult = SHDeleteKey(hRoot, ptzSubKey);

	if ((lResult == ERROR_SUCCESS) || (lResult == ERROR_FILE_NOT_FOUND) ||
		(lResult == ERROR_PATH_NOT_FOUND))
	{
		return S_OK;
	}
	return (HRESULT) lResult;
}

// The read half. *pcbData is in/out, exactly as RegQueryValueEx wants it, so a
// caller can ask for the size first and the data second. A key or a value that
// is not there reports the Win32 code (ERROR_FILE_NOT_FOUND), which is what
// lets a caller tell "absent" from "present but empty".
HRESULT RegQueryValueRaw(HKEY hRoot, PCTSTR ptzSubKey, PCTSTR ptzName,
	DWORD *pdwType, void *pvData, DWORD *pcbData)
{
	HKEY hKey;
	LONG lResult;

	lResult = RegOpenKeyEx(hRoot, ptzSubKey, 0, KEY_QUERY_VALUE, &hKey);
	if (lResult != ERROR_SUCCESS)
	{
		return (HRESULT) lResult;
	}
	lResult = RegQueryValueEx(hKey, ptzName, NULL, pdwType, (LPBYTE) pvData, pcbData);
	RegCloseKey(hKey);
	return (HRESULT) lResult;
}

// Loading an offline hive and taking it back down. The mount name is a bare
// key name under HKLM. The legacy dialect has no command for this at all, which
// is why rewriting a boot entry used to mean shelling out to reg.exe, and why
// this is a new capability rather than a moved one.
//
// It needs SeBackup/SeRestore, so it works as SYSTEM in the PE and not as an
// ordinary user.
HRESULT RegLoadHive(PCTSTR ptzFile, PCTSTR ptzMount)
{
	// Turn the two privileges on first. The token holds them already in a PE,
	// but they start disabled, and RegLoadKey answers ERROR_PRIVILEGE_NOT_HELD
	// rather than enabling them for you -- which reads like a permission problem
	// when it is only a switch nobody flipped.
	//
	// Priv() lives in display.cpp, which the area split placed after this file.
	// The build is one translation unit, so a declaration is enough.
	HRESULT Priv(PCTSTR ptzName);
	Priv(SE_BACKUP_NAME);
	Priv(SE_RESTORE_NAME);

	LONG lResult = RegLoadKey(HKEY_LOCAL_MACHINE, (PTSTR) ptzMount, ptzFile);

	if (lResult == ERROR_SUCCESS)
	{
		RememberLoadedHive(ptzMount);
	}
	return (lResult == ERROR_SUCCESS) ? S_OK : (HRESULT) lResult;
}

HRESULT RegUnloadHive(PCTSTR ptzMount)
{
	LONG lResult = RegUnLoadKey(HKEY_LOCAL_MACHINE, (PTSTR) ptzMount);

	if (lResult == ERROR_SUCCESS)
	{
		ForgetLoadedHive(ptzMount);
	}
	return (lResult == ERROR_SUCCESS) ? S_OK : (HRESULT) lResult;
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// REGI <root>\<subkey>\<name>[=<data>]
//
// The '=' is what makes it a write. Without one the reference wants a
// destination variable (a read) or an operator (a delete); with neither it
// answers E_INVALIDARG -- a bare path must not quietly delete, which is what
// difference 005 was. The first '\' separates the root from the rest, and the
// last '\' in the rest separates the subkey from the value name.
//
// The text is copied into local buffers rather than cut up where it stands: the
// caller's line already has a trace copy of its own, but nothing here needs to
// write into it.
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
//  Folder macro
#include <ShlObj.h>
const struct {INT iFolder; PCTSTR ptzMacro;} c_sMacro[] =
{
	{CSIDL_FAVORITES,			TEXT("Favorites")},
	{CSIDL_DESKTOPDIRECTORY,	TEXT("Desktop")},
	{CSIDL_STARTMENU,			TEXT("StartMenu")},
	{CSIDL_STARTUP,				TEXT("Startup")},
	{CSIDL_PROGRAMS,			TEXT("Programs")},
	{CSIDL_SENDTO,				TEXT("SendTo")},
	{CSIDL_PERSONAL,			TEXT("Personal")},
	{CSIDL_APPDATA,				TEXT("QuickLaunch")},
};
//////////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////////
#define Text_Environment TEXT("Environment")
#define REG_MemMgr TEXT("SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management")
#define Session_Manager TEXT("SYSTEM\\CurrentControlSet\\Control\\Session Manager")
// What this does, in the three shapes its callers use:
//
//   an empty name with bInit    establish the built-in directory variables
//                               (%Desktop%, %Programs%, ...) from the shell's
//                               own special-folder paths
//   a name with bSystem         put it in the machine and user environment keys
//                               as well as in this process
//   anything else               set the process environment variable, and tell
//                               the world the environment changed
//
// The signature keeps its default arguments: twenty call sites pass one, two or
// three of them.
HRESULT SetEnvironment(PTSTR ptzEnv, BOOL bSystem = FALSE,BOOL bInit = FALSE)
{
	TCHAR tzStr[MAX_STR];
	HRESULT hResult = S_OK;

	if (!ptzEnv[0])
	{
		if (bInit)
		{
			for (UINT i = 0; i < _NumOf(c_sMacro); i++)
			{
				UStrPrint(tzStr, TEXT("%s="), c_sMacro[i].ptzMacro);
				PTSTR ptzPath = tzStr + UStrLen(tzStr);

				if (!SHGetSpecialFolderPath(NULL, ptzPath, c_sMacro[i].iFolder, TRUE))
				{
					continue;
				}

				if (c_sMacro[i].iFolder == CSIDL_APPDATA)
				{
					// "QuickLaunch" is not a folder the shell reports on its
					// own; it lives under the roaming app data one.
					UStrCat(ptzPath, TEXT("\\Microsoft\\Internet Explorer\\Quick Launch"));
					UDirCreate(ptzPath);
					CreateDirectory(ptzPath, NULL);
				}

				SetEnvironment(tzStr, bSystem,FALSE);
			}
		}
		else
		{
			DWORD_PTR dwResult = 0;

			SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)Text_Environment, SMTO_ABORTIFHUNG, 5000, &dwResult);
		}
	}
	else
	{
		if (bSystem)
		{
			// The machine and user keys, so the value is there for the next
			// process and not only for this one. The name is the part before the
			// '=', and the write goes straight to the two keys -- the text form
			// this used to go through (REGI with a "root\key\name=value" line) is
			// gone with the dialect it belonged to.
			PTSTR ptzEq = UStrChr(ptzEnv, '=');

			if (ptzEq)
			{
				TCHAR tzName[MAX_NAME];
				PTSTR ptzValue = ptzEq + 1;
				DWORD cbValue = (DWORD) ((UStrLen(ptzValue) + 1) * sizeof(TCHAR));
				HRESULT hrKey;

				// A name that cannot fit is refused rather than cut short: half a
				// name is a different variable, and the write would still report
				// success.
				if ((UINT) (ptzEq - ptzEnv) >= _NumOf(tzName))
				{
					return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
				}
				UStrCopyN(tzName, ptzEnv, (UINT) (ptzEq - ptzEnv) + 1);
				UStrPrint(tzStr, TEXT("%s\\%s"), Session_Manager, Text_Environment);
				hrKey = RegSetValue(HKEY_LOCAL_MACHINE, tzStr, tzName, REG_SZ, ptzValue, cbValue);
				if (hrKey == S_OK)
				{
					hrKey = RegSetValue(HKEY_CURRENT_USER, Text_Environment, tzName, REG_SZ, ptzValue, cbValue);
				}
				// The registry half is what a *later* process reads, and the
				// process environment below is what this one and its children
				// read. A failure in either is worth reporting; the caller that
				// asked for both gets the one that failed. The value is a Win32
				// code rather than a signed HRESULT, so "not S_OK" is the test.
				if (hrKey != S_OK)
				{
					hResult = hrKey;
				}
			}
		}

		PTSTR ptzValue = UStrChr(ptzEnv, '=');
		if (ptzValue)
		{
			// "name=value" is what gets set; a name on its own is a constant
			// string, which the callers that pass one use only for the keys
			// written just above.
			*ptzValue++ = 0;
			if (!SetEnvironmentVariable(ptzEnv, ptzValue) && (hResult == S_OK))
			{
				hResult = S_FALSE;
			}
		}
	}

	if (bSystem)
	{
		DWORD_PTR dwResult = 0;

		SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)Text_Environment, SMTO_ABORTIFHUNG, 5000, &dwResult);
	}
	return hResult;
}
//////////////////////////////////////////////////////////////////////////////////////////////////


// The two-layer variable system -- a process value for children, and a stored one
// for the machine -- is `env.set` now; the ENVI/SET spelling, and the PE-variable
// prefix system they were split across, went with the text dialect.
//////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////
// The logo dialog's layered-window call needs no fallback here: the whole
// hand-loaded-pointer dance that used to live at this spot only ran on SDKs that
// did not define LWA_COLORKEY, and this toolchain always does.






///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
