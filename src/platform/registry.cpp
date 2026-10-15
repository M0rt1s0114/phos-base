//////////////////////////////////////////////////////////////////////////////////////////////////
// The registry capabilities
//
// These are the operations themselves: a root key, a subkey, a value name and
// typed data. Regi() below is the text front end for the same
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

HRESULT RegDeleteTree(HKEY hRoot, PCTSTR ptzSubKey)
{
	LONG lResult = SHDeleteKey(hRoot, ptzSubKey);

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
// key name under HKLM. The text-command dialect has no command for this at all,
// which is why rewriting a boot entry has always meant shelling out to reg.exe.
// This is a new capability rather than a moved one.
//
// It needs SeBackup/SeRestore, so it works as SYSTEM in the PE and not as an
// ordinary user.
HRESULT RegLoadHive(PCTSTR ptzFile, PCTSTR ptzMount)
{
	LONG lResult = RegLoadKey(HKEY_LOCAL_MACHINE, (PTSTR) ptzMount, ptzFile);

	return (lResult == ERROR_SUCCESS) ? S_OK : (HRESULT) lResult;
}

HRESULT RegUnloadHive(PCTSTR ptzMount)
{
	LONG lResult = RegUnLoadKey(HKEY_LOCAL_MACHINE, (PTSTR) ptzMount);

	return (lResult == ERROR_SUCCESS) ? S_OK : (HRESULT) lResult;
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// REGI <root>\<subkey>\<name>[=<data>]
//
// The '=' is what makes it a write. Without one the text form wants a
// destination variable (a read) or an operator (a delete); with neither it
// answers E_INVALIDARG -- a bare path must not quietly delete something. The
// first '\' separates the root from the rest, and the last '\' in the rest
// separates the subkey from the value name.
//
// The text is copied into local buffers rather than cut up where it stands: the
// caller's line already has a trace copy of its own, but nothing here needs to
// write into it.
HRESULT Regi(CoreContext& ctx, PTSTR ptzCmd)
{
	TCHAR tzRoot[MAX_NAME];
	TCHAR tzSubKey[MAX_STR];
	TCHAR tzData[MAX_STR] = {0};
	BOOL bWrite = FALSE;

	PTSTR ptzSplit = UStrChr(ptzCmd, '\\');
	if (!ptzSplit)
	{
		return E_INVALIDARG;
	}
	// The root name, checked against the buffer it goes into: the count handed
	// to UStrCopyN is how much to copy, not how much room there is, so a long
	// root name ran over the end of tzRoot.
	if (((size_t) (ptzSplit - ptzCmd) + 1) > _NumOf(tzRoot))
	{
		return E_INVALIDARG;
	}
	UStrCopyN(tzRoot, ptzCmd, (UINT) (ptzSplit - ptzCmd) + 1);

	HKEY hKey = RegRootFromName(tzRoot);
	if (!hKey)
	{
		return E_INVALIDARG;
	}

	// The '=' splits the value name from the data. An empty data part is still a
	// write rather than a query -- the boot configuration ends wallpaper.ini
	// with exactly that form -- and the write is a removal, at the end of this
	// function.
	UStrCopy(tzSubKey, ptzSplit + 1);
	ptzSplit = UStrChr(tzSubKey, '=');
	if (ptzSplit)
	{
		*ptzSplit++ = 0;
		UStrCopy(tzData, UStrTrim(ptzSplit));
		bWrite = TRUE;
	}

	ptzSplit = UStrRChr(tzSubKey, '\\');
	if (!ptzSplit)
	{
		return E_INVALIDARG;
	}
	*ptzSplit++ = 0;

	// The value name stays a pointer into tzSubKey, which this function owns.
	// Registry value names are longer than any fixed name buffer we keep.
	PTSTR ptzValue = ptzSplit;

	if (!bWrite)
	{
		PTSTR ptzVar;

		// "!" where the value name would be means "the whole subkey".
		if ((ptzValue[0] == '!') && (ptzValue[1] == 0))
		{
			return RegDeleteTree(hKey, tzSubKey);
		}

		// Read form: "REGI <key>,<variable>". The variable says where the value
		// goes, and "&" makes it local exactly as it does everywhere else in this
		// dialect. Only a string is readable this way -- the text form answers
		// E_INVALIDARG for a DWORD -- so the type is checked before the value is
		// handed over.
		ptzVar = UStrChr(ptzValue, ',');
		if (ptzVar)
		{
			TCHAR tzValueData[MAX_STR] = {0};
			DWORD dwType = 0;
			DWORD cbData = sizeof(tzValueData);
			HRESULT hr;

			*ptzVar++ = 0;
			hr = RegQueryValueRaw(hKey, tzSubKey, ptzValue, &dwType, tzValueData, &cbData);
			if (hr != S_OK)
			{
				return hr;
			}
			if (dwType != REG_SZ)
			{
				return E_INVALIDARG;
			}
			return PeVar::Set(ptzVar, tzValueData) ? S_OK : S_FALSE;
		}

		return E_INVALIDARG;
	}

	if (tzData[0] == '#')
	{
		DWORD dwData = UStrToInt(tzData + 1);

		return RegSetValue(hKey, tzSubKey, ptzValue, REG_DWORD, &dwData, sizeof(DWORD));
	}

	if (tzData[0] == '@')
	{
		// "@49 50 51" is the bytes 49 / 50 / 51 in decimal, space separated.
		BYTE bData[MAX_PATH];
		UINT uByte = 0;
		PTSTR ptzByte = tzData + 1;

		do
		{
			PTSTR ptzNext;

			bData[uByte++] = (BYTE) UStrToInt(ptzByte);
			ptzNext = UStrChr(ptzByte, ' ');
			if (!ptzNext)
			{
				break;
			}
			ptzByte = ptzNext + 1;
		}
		while (uByte < _NumOf(bData));

		return RegSetValue(hKey, tzSubKey, ptzValue, REG_BINARY, bData, uByte);
	}

	// An empty right-hand side removes the value rather than storing an empty
	// string, and it answers success either way. The only way to see what
	// happened is to read the value back: it is not there afterwards. Handing
	// SHSetValue no data at all is not the same thing -- that stores an empty
	// value instead of removing one.
	if (tzData[0] == 0)
	{
		return RegDeleteValue(hKey, tzSubKey, ptzValue);
	}

	return RegSetValue(hKey, tzSubKey, ptzValue, REG_SZ, tzData, UStrLen(tzData) * sizeof(TCHAR));
}
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
			// process and not only for this one.
			UStrPrint(tzStr, TEXT("HKLM\\%s\\%s\\%s"),Session_Manager,Text_Environment,ptzEnv);
			Regi(g_ctx, tzStr);      // machine-wide
			UStrPrint(tzStr, TEXT("%s\\%s\\%s"),hKCU,Text_Environment,ptzEnv);
			Regi(g_ctx, tzStr);      // and the user's
		}

		PTSTR ptzValue = UStrChr(ptzEnv, '=');
		if (ptzValue)
		{
			// "name=value" is what gets set; a name on its own is a constant
			// string, which the callers that pass one use only for the keys
			// written just above.
			*ptzValue++ = 0;
			hResult = SetEnvironmentVariable(ptzEnv, ptzValue) ? S_OK : S_FALSE;
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


///////////////////////////////////////////////////
// Where ENVI and SET put their value
//
//   ENVI  name=value    the process environment, unless ENVI^ ForceLocal=1
//   ENVI &name=value    a PE variable, whatever the mode says
//   ENVI $name=value    system level: registry plus the environment
//   SET   name=value    always a PE variable, created local
//
// The two halves never touch each other's storage, so a script can keep them
// apart instead of guessing which layer a name ended up in. That separation is
// the whole point of the prefix system.
///////////////////////////////////////////////////
static HRESULT EnviSwitch(PTSTR ptzCmd)
{
	// ENVI^ ForceLocal=1. Only the scope switch is implemented so far; the
	// expansion modes that go with EnviMode are not.
	PTSTR ptzName = UStrTrim(ptzCmd);
	PTSTR ptzValue = UStrChr(ptzName, '=');

	if (!ptzValue)
	{
		return S_OK;
	}
	*ptzValue++ = 0;
	UStrTrim(ptzName);

	if (!UStrCmpI(ptzName, TEXT("ForceLocal")))
	{
		PeVar::SetForceLocal(UStrToInt(UStrTrim(ptzValue)) != 0);
	}
	return S_OK;
}

HRESULT Envi(CoreContext& ctx, PTSTR ptzCmd)
{
	BOOL bSys;
	PTSTR ptzName;
	PTSTR ptzValue;

	if (*ptzCmd == '^')
	{
		return EnviSwitch(ptzCmd + 1);
	}

	bSys = (*ptzCmd == '$');
	ptzCmd += bSys ? 1 : 0;

	if (PeVar::IsPeName(ptzCmd) || ((!bSys) && PeVar::ForceLocal()))
	{
		ptzName = UStrTrim(ptzCmd);
		ptzValue = UStrChr(ptzName, '=');
		if (!ptzValue)
		{
			return S_OK;		// query forms come with the rest of the surface
		}
		*ptzValue++ = 0;
		UStrTrim(ptzValue);
		UStrTrim(ptzName);

		if (PeVar::IsPeName(ptzName))
		{
			return PeVar::Set(ptzName, ptzValue) ? S_OK : S_FALSE;
		}

		// ForceLocal turned a bare name into a PE variable.
		TCHAR tzName[MAX_NAME];

		if ((UStrLen(ptzName) + 3) >= _NumOf(tzName))
		{
			return E_INVALIDARG;
		}
		UStrPrint(tzName, TEXT("&&%s"), ptzName);
		return PeVar::Set(tzName, ptzValue) ? S_OK : S_FALSE;
	}

	return SetEnvironment(ptzCmd,bSys,((!ptzCmd) && bSys));
}
///////////////////////////////////////////////////
//Set a PE variable
HRESULT SetVar(CoreContext& ctx, PTSTR ptzCmd)
{
	// SET is ENVI & by definition: always a PE variable, never the environment.
	// A bare name becomes a local; explicit &, && and &:: prefixes win.
	PTSTR ptzName;
	PTSTR ptzValue;
	TCHAR tzName[MAX_NAME];

	if (*ptzCmd == '-')
	{
		return S_OK;			// SET-def and friends come later
	}

	ptzName = ptzCmd;
	ptzValue = UStrChr(ptzName, '=');
	if (!ptzValue)
	{
		return S_OK;
	}
	*ptzValue++ = 0;
	UStrTrim(ptzValue);
	ptzName = UStrTrim(ptzName);

	if (PeVar::IsPeName(ptzName))
	{
		return PeVar::Set(ptzName, ptzValue) ? S_OK : S_FALSE;
	}

	if ((UStrLen(ptzName) + 3) >= _NumOf(tzName))
	{
		return E_INVALIDARG;
	}
	UStrPrint(tzName, TEXT("&&%s"), ptzName);
	if (!PeVar::Set(tzName, ptzValue))
	{
		return S_FALSE;
	}
	// A bare name is written to the environment as well. That is not in the
	// documentation, but it is observable: FlashPic.WCS is on the PE's own boot
	// path, it does "set FlashPic=Logo%RanPic%.jpg" and then uses
	// "%FlashPic%" - an expansion that only ever consults the environment - and
	// the text form resolves it. The &-prefixed forms stay PE-only, which is
	// what the prefix is for.
	{
		TCHAR tzPair[MAX_STR];
		UStrPrint(tzPair, TEXT("%s=%s"), ptzName, ptzValue);
		SetEnvironment(tzPair, FALSE, FALSE);
	}
	return S_OK;
}
///////////////////////////////////////////////////
