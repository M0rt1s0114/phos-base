// The file, shortcut, process-ending and DLL capabilities, and their text front
// ends (FILE / LINK / SEND / KILL / SHUT / CALL).
//
// FsDelete, DirCreate, FsReadAll, FsWriteAll, FsRename, FsHardLink, FsTempDir,
// CreateShortcut, KillProc and NativeCall do the work those front ends call.

// "FILE <path>" deletes. "FILE <from>=><to>" copies and "FILE <from>-><to>"
// moves.
//
// Only the two-character operators count: a bare ">" ends the first path and is
// not an operator, which is what the text form does with it.
HRESULT File(CoreContext& ctx, PTSTR ptzCmd)
{
	TCHAR tzFrom[MAX_STR];
	TCHAR tzTo[MAX_STR];
	UINT uFunc = FO_DELETE;

	UStrCopy(tzFrom, ptzCmd);
	tzTo[0] = 0;

	PTSTR ptzMark = UStrChr(tzFrom, '>');

	if (ptzMark && (ptzMark > tzFrom))
	{
		TCHAR cOp = ptzMark[-1];

		*ptzMark = 0;
		if ((cOp == '=') || (cOp == '-'))
		{
			ptzMark[-1] = 0;
			UStrCopy(tzTo, ptzMark + 1);
			uFunc = (cOp == '=') ? FO_COPY : FO_MOVE;
		}
	}

	// SHFileOperation wants double-NUL terminated lists, and the buffers have to
	// be ours. The second NUL goes into the end of the copy built here, not one
	// past the end of the caller's string.
	std::vector<TCHAR> vFrom;
	std::vector<TCHAR> vTo;
	SHFILEOPSTRUCT so = {0};

	vFrom.assign(tzFrom, tzFrom + UStrLen(tzFrom));
	vFrom.push_back(0);
	vFrom.push_back(0);

	so.pFrom = vFrom.data();
	so.wFunc = uFunc;
	so.fFlags = FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI | FOF_SILENT;

	if (uFunc != FO_DELETE)
	{
		vTo.assign(tzTo, tzTo + UStrLen(tzTo));
		vTo.push_back(0);
		vTo.push_back(0);
		so.pTo = vTo.data();
	}

	return SHFileOperation(&so);
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// The file capabilities
//
// The operations themselves. File() above is the text front end for the
// same two things (delete, and copy/move through its "=>" / "->" syntax); the
// language's fs.* verbs call these instead of building "FILE ..." strings.
//////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT FsDelete(PCTSTR ptzPath)
{
	SHFILEOPSTRUCT so = {0};
	std::vector<TCHAR> from;
	int iResult;

	// SHFileOperation wants a double-NUL terminated list of names, on a buffer
	// that belongs to this call.
	from.assign(ptzPath, ptzPath + UStrLen(ptzPath));
	from.push_back(0);
	from.push_back(0);

	so.pFrom = from.data();
	so.wFunc = FO_DELETE;
	so.fFlags = FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI | FOF_SILENT;

	iResult = SHFileOperation(&so);
	return (iResult == 0) ? S_OK : (HRESULT) iResult;
}

HRESULT DirCreate(PCTSTR ptzPath)
{
	// UDirCreate makes the intermediate levels; CreateDirectory then either
	// makes the leaf or says why it could not.
	UDirCreate((PTSTR) ptzPath);
	return CreateDirectory(ptzPath, NULL) ? S_OK : (HRESULT) GetLastError();
}

// The rest of the file capability: reading, writing, and the two operations the
// text commands spell with flags. READ and WRIT are a line-oriented,
// encoding-tagged text format; these take and return values, and the host turns
// them into what the language has (a str, a list, a count).
HRESULT FsReadAll(PCTSTR ptzPath, std::vector<BYTE> *pData)
{
	HANDLE hFile = CreateFile(ptzPath, GENERIC_READ, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	if (hFile == INVALID_HANDLE_VALUE)
	{
		return (HRESULT) GetLastError();
	}
	{
		BYTE abChunk[4096];

		for (;;)
		{
			DWORD cbChunk = 0;

			if (!ReadFile(hFile, abChunk, sizeof(abChunk), &cbChunk, NULL))
			{
				HRESULT hr = (HRESULT) GetLastError();
				CloseHandle(hFile);
				return hr;
			}
			if (cbChunk == 0)
			{
				break;
			}
			pData->insert(pData->end(), abChunk, abChunk + cbChunk);
		}
	}
	CloseHandle(hFile);
	return S_OK;
}

HRESULT FsWriteAll(PCTSTR ptzPath, const void *pvData, DWORD cbData, BOOL bAppend)
{
	HANDLE hFile = CreateFile(ptzPath, GENERIC_WRITE, 0, NULL,
		bAppend ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD cbWritten = 0;

	if (hFile == INVALID_HANDLE_VALUE)
	{
		return (HRESULT) GetLastError();
	}
	if (bAppend)
	{
		SetFilePointer(hFile, 0, NULL, FILE_END);
	}
	if ((cbData > 0) && !WriteFile(hFile, pvData, cbData, &cbWritten, NULL))
	{
		HRESULT hr = (HRESULT) GetLastError();
		CloseHandle(hFile);
		return hr;
	}
	CloseHandle(hFile);
	return S_OK;
}

HRESULT FsRename(PCTSTR ptzFrom, PCTSTR ptzTo)
{
	// COPY_ALLOWED so a move lands even when the two names are on volumes that
	// cannot be renamed across, which is what the text form does.
	return MoveFileEx(ptzFrom, ptzTo, MOVEFILE_COPY_ALLOWED) ? S_OK : (HRESULT) GetLastError();
}

HRESULT FsHardLink(PCTSTR ptzLink, PCTSTR ptzTarget)
{
	return CreateHardLink(ptzLink, ptzTarget, NULL) ? S_OK : (HRESULT) GetLastError();
}

HRESULT FsTempDir(PTSTR ptzBuf, DWORD cchBuf)
{
	DWORD cch = GetTempPath(cchBuf, ptzBuf);

	return (cch > 0) ? S_OK : (HRESULT) GetLastError();
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// Create shortcut
// The shortcut capability. Link() below is the text front end for it.
//
// The text form packs everything into one comma-separated string and mutates
// it in place; the structured form takes the pieces as arguments, which is the
// point -- no comma, no quoting rule, and no buffer to write past the end of.
//
// bVerbatimTarget keeps the text form's '*' prefix working: without it the target is
// resolved the way Explorer would resolve it. The structured verbs never ask
// for it, because a caller that wants a literal path can pass one that exists.
HRESULT CreateShortcut(PCTSTR ptzLnk, PCTSTR ptzTarget, PCTSTR ptzArgs, PCTSTR ptzIcon,
	INT iIcon, BOOL bMinimized, BOOL bVerbatimTarget)
{
	IShellLink *pLink;
	HRESULT hResult;
	TCHAR tzTarget[MAX_PATH];
	TCHAR tzIcon[MAX_PATH];

	if (!bVerbatimTarget)
	{
		if (SearchPath(NULL, ptzTarget, NULL, MAX_PATH, tzTarget, NULL))
		{
			ptzTarget = tzTarget;
		}
		else if (!UDirExist((PTSTR) ptzTarget))
		{
			return ERROR_PATH_NOT_FOUND;
		}
	}

	// An icon given as a bare name ("shell32.dll") is looked up the same way
	// the target is, because Explorer resolves szIconLocation on its own and
	// cannot do anything with a relative path. Stored verbatim, it silently
	// meant no icon.
	if ((ptzIcon != NULL) && (ptzIcon[0] != 0) && !UFileExist((PTSTR) ptzIcon) &&
		SearchPath(NULL, ptzIcon, NULL, MAX_PATH, tzIcon, NULL))
	{
		ptzIcon = tzIcon;
	}

	CoInitialize(NULL);
	hResult = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLink, (PVOID *) &pLink);
	if (hResult == S_OK)
	{
		IPersistFile *pFile;

		hResult = pLink->QueryInterface(IID_IPersistFile, (PVOID *) &pFile);
		if (hResult == S_OK)
		{
			WCHAR wzLink[MAX_PATH];
			TCHAR tzLink[MAX_PATH];
			TCHAR tzWork[MAX_PATH];

			if (bMinimized)
			{
				hResult = pLink->SetShowCmd(SW_SHOWMINIMIZED);
			}
			hResult = pLink->SetPath((PTSTR) ptzTarget);
			hResult = pLink->SetArguments((PTSTR) ptzArgs);
			hResult = pLink->SetIconLocation((PTSTR) ptzIcon, iIcon);

			// "Start in the target's folder" -- but only when the target has
			// one. UDirSplitPath truncates in place, hence the copy.
			UStrCopy(tzWork, (PTSTR) ptzTarget);
			if (UDirSplitPath(tzWork) != tzWork)
			{
				hResult = pLink->SetWorkingDirectory(tzWork);
			}

			UStrPrint(tzLink, TEXT("%s.LNK"), ptzLnk);
			UStrToWStr(wzLink, tzLink, MAX_PATH);
			UDirCreate(tzLink);
			hResult = pFile->Save(wzLink, FALSE);

			pFile->Release();
		}
		pLink->Release();
	}
	CoUninitialize();
	return hResult;
}

// Parse Shortcut,Target,Param,IconPath,IconIndex
HRESULT Link(CoreContext& ctx, PTSTR ptzCmd)
{
	// Shortcut,Target,Param,IconPath,IconIndex. Every field after the first is
	// optional. The separators are NUL'd in place, which is safe here: the
	// buffer belongs to Command() and the trace already has its own copy.
	PTSTR ptzTarget = UStrChr(ptzCmd, ',');
	INT iIcon = 0;
	PTSTR ptzIcon = NULL;
	PTSTR ptzParam = NULL;
	BOOL bMinimized = FALSE;
	BOOL bVerbatim = FALSE;

	if (ptzTarget == NULL)
	{
		return ERROR_PATH_NOT_FOUND;
	}
	*ptzTarget++ = 0;

	ptzParam = UStrChr(ptzTarget, ',');
	if (ptzParam)
	{
		*ptzParam++ = 0;
		ptzIcon = UStrChr(ptzParam, ',');
		if (ptzIcon)
		{
			PTSTR ptzIndex;

			*ptzIcon++ = 0;
			ptzIndex = UStrChr(ptzIcon, ',');
			if (ptzIndex)
			{
				*ptzIndex++ = 0;
				iIcon = UStrToInt(ptzIndex);
			}
		}
	}

	// The two prefixes belong to the shortcut path. The structured verb carries
	// them as arguments instead, which is why it has no spelling for them.
	if (*ptzCmd == '*')
	{
		ptzCmd++;
		bVerbatim = TRUE;
	}
	if (*ptzCmd == '!')
	{
		ptzCmd++;
		bMinimized = TRUE;
	}

	return CreateShortcut(ptzCmd, ptzTarget, ptzParam, ptzIcon, iIcon, bMinimized, bVerbatim);
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// Synthesizes a keystroke
HRESULT Send(CoreContext& ctx, PTSTR ptzCmd)
{
	// Comma separated virtual key codes. A "^" after a code means "press only",
	// a "_" means "release only", and anything else presses and releases.
	INT iLast = 0;
	PTSTR ptzField = ptzCmd;

	for (;;)
	{
		PTSTR ptzSep = UStrChr(ptzField, ',');
		TCHAR cSuffix = 0;

		// The suffix belongs to this field, so it is read before the field is
		// parsed. An empty field has none: reading a character before the field
		// would walk off the front of the line.
		if (ptzSep && (ptzSep > ptzField))
		{
			cSuffix = ptzSep[-1];
		}

		iLast = UStrToInt(ptzField);
		if (cSuffix != '^')
		{
			keybd_event(iLast, 0, 0, 0);
		}
		if (cSuffix != '_')
		{
			keybd_event(iLast, 0, KEYEVENTF_KEYUP, 0);
		}

		if (!ptzSep)
		{
			break;
		}
		ptzField = ptzSep + 1;
	}

	return iLast ? S_OK : S_FALSE;
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// Find process
HRESULT FindProc(PCTSTR ptzCmd)
{
	// With a name: the first process whose image name starts with it, which is
	// how "explorer" stands for "explorer.exe". With no name: this process's
	// parent. Zero when there is no answer.
	DWORD dwFound = 0;
	DWORD dwSelf = GetCurrentProcessId();
	HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

	if (hSnap == INVALID_HANDLE_VALUE)
	{
		return 0;
	}
	{
		PROCESSENTRY32 pe;

		pe.dwSize = sizeof(PROCESSENTRY32);
		for (BOOL b = Process32First(hSnap, &pe); b; b = Process32Next(hSnap, &pe))
		{
			if (ptzCmd)
			{
				if (!UStrCmpNI(pe.szExeFile, ptzCmd, UStrLen(ptzCmd)))
				{
					dwFound = pe.th32ProcessID;
					break;
				}
			}
			else if (pe.th32ProcessID == dwSelf)
			{
				dwFound = pe.th32ParentProcessID;
				break;
			}
		}
	}
	CloseHandle(hSnap);
	return dwFound;
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// The process-ending capability. Every process whose image name matches goes;
// the answer is S_OK when at least one was terminated and S_FALSE when none
// was found. S_FALSE rather than an error is deliberate: the boot chain is
// allowed to kill something that is not running, and the trace records it.
HRESULT KillProc(PCTSTR ptzName)
{
	HRESULT hResult = S_FALSE;
	DWORD ProcessID = 0;

	if (!ptzName)
	{
		return S_OK;
	}
	do
	{
		ProcessID = FindProc((PTSTR) ptzName);
		if (ProcessID)
		{
			HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, ProcessID);
			if (hProcess)
			{
				if (TerminateProcess(hProcess, 0))
				{
					hResult = S_OK;
				}
				CloseHandle(hProcess);
			}
		}
	}
	while (ProcessID > 0);

	return hResult;
}

HRESULT Kill(CoreContext& ctx, PTSTR ptzCmd)
{
	return KillProc(ptzCmd);
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT Boot(BOOL bReboot)
{
	// Tell everyone first, the way the text form does, then ask the kernel. The
	// notifications are best effort: nothing reads what came back.
	DWORD_PTR dwResult;
	typedef DWORD (NTAPI *PNtShutdownSystem)(DWORD dwAction);
	PNtShutdownSystem NtShutdownSystem;

	SendMessageTimeout(HWND_BROADCAST, WM_QUERYENDSESSION, 0, 0, 0, 2000, &dwResult);
	SendMessageTimeout(HWND_BROADCAST, WM_ENDSESSION, 0, 0, 0, 2000, &dwResult);
	SendMessageTimeout(HWND_BROADCAST, WM_DESTROY, 0, 0, 0, 2000, &dwResult);

	// NtShutdownSystem is in no import library, so it is looked up by name.
	NtShutdownSystem = (PNtShutdownSystem) GetProcAddress(GetModuleHandle(TEXT("NTDLL")), "NtShutdownSystem");
	if (!NtShutdownSystem)
	{
		return E_FAIL;
	}

	Priv(SE_SHUTDOWN_NAME);

	// 1 reboots, 2 shuts down. That mapping is the documented one, and the tracer
	// cannot see it: this call ends the run.
	return NtShutdownSystem(bReboot ? 1: 2);
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// Shutdown
HRESULT Shut(CoreContext& ctx, PTSTR ptzCmd)
{
	// "SHUT R" reboots; anything else falls back to the flag the shell left in
	// g_iShowCmd, which is how the desktop's own shutdown reaches here.
	BOOL bReboot = ((ptzCmd[0] == 'R') || (ptzCmd[0] == 'r')) || (g_iShowCmd & EWX_REBOOT);

	return Boot(bReboot);
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// Call DLL function
typedef HRESULT (WINAPI *PROC1)(PVOID pv0);
typedef HRESULT (WINAPI *PROC2)(PVOID pv0, PVOID pv1);
typedef HRESULT (WINAPI *PROC3)(PVOID pv0, PVOID pv1, PVOID pv2);
typedef HRESULT (WINAPI *PROC4)(PVOID pv0, PVOID pv1, PVOID pv2, PVOID pv3);
// The DLL capability.
//
// *plResult is the called function's own return value. The HRESULT says
// whether the call could be made at all: S_OK when it was, ERROR_MOD_NOT_FOUND
// when the library would not load, ERROR_PROC_NOT_FOUND when the export is not
// there. Keeping those apart is the whole point -- "the library would not load"
// and "the function answered 126" are different things, and the text form's
// CALL $ deliberately reports the first one as success.
HRESULT NativeCall(PCTSTR ptzLibrary, PCTSTR ptzProc, PVOID *ppvArgs, UINT uArgs, LONG *plResult)
{
	HMODULE hLib;
	CHAR szProc[MAX_NAME];
	FARPROC f;

	*plResult = 0;
	hLib = LoadLibrary(ptzLibrary);
	if (!hLib)
	{
		return (HRESULT) ERROR_MOD_NOT_FOUND;
	}
	if (!ptzProc)
	{
		ptzProc = TEXT("DllRegisterServer");
	}
	// CALL $dll,* is preload only: the text form keeps the module loaded so a
	// later call is cheaper, so this path deliberately does not free it.
	if (!UStrCmp(ptzProc, TEXT("*")))
	{
		return S_OK;
	}
	UStrToAStr(szProc, (PTSTR) ptzProc, MAX_NAME);
	f = GetProcAddress(hLib, szProc);
	if (!f)
	{
		FreeLibrary(hLib);
		return (HRESULT) ERROR_PROC_NOT_FOUND;
	}
	switch (uArgs)
	{
	case 0: *plResult = (LONG) f(); break;
	case 1: *plResult = (LONG) ((PROC1) f)(ppvArgs[0]); break;
	case 2: *plResult = (LONG) ((PROC2) f)(ppvArgs[0], ppvArgs[1]); break;
	case 3: *plResult = (LONG) ((PROC3) f)(ppvArgs[0], ppvArgs[1], ppvArgs[2]); break;
	case 4: *plResult = (LONG) ((PROC4) f)(ppvArgs[0], ppvArgs[1], ppvArgs[2], ppvArgs[3]); break;
	default:
		FreeLibrary(hLib);
		return E_INVALIDARG;
	}
	FreeLibrary(hLib);
	return S_OK;
}

// The text front end. The field after the library is the *function name*, and
// each field after a following comma is an argument.
HRESULT WINAPI CallDll(PTSTR ptzCmd)
{
	// "dll[,function][,arg]*". The field after the library is the *name*, and
	// each field after a following comma is an argument -- which is why the
	// search starts at the name and not after it. Walking fields from the name
	// instead made the name itself argument zero, and "CALL $gdi32.dll,EnableEUDC"
	// then called a function that takes none with a string pointer; that is what
	// took the boot chain down.
	UINT uArg = 0;
	PVOID pvArg[4] = {0};
	PTSTR ptzProc = UStrChr(ptzCmd, ',');
	LONG lResult = 0;
	HRESULT hr;

	if (ptzProc)
	{
		*ptzProc++ = 0;
		for (PTSTR p = ptzProc; (uArg < _NumOf(pvArg)) && (p = UStrChr(p, ',')); uArg++)
		{
			*p++ = 0;
			pvArg[uArg] = (PVOID) p;
			if (*p == '#')
			{
				// "#7" is the integer 7, not a pointer to the text.
				pvArg[uArg] = (PVOID)(INT_PTR) UStrToInt(p + 1);
			}
		}
	}

	hr = NativeCall(ptzCmd, ptzProc, pvArg, uArg, &lResult);
	if (hr != S_OK)
	{
		// What the text form reports here, and the trace records: a library
		// that will not load is success, a missing export is the Win32 code.
		// browseui.dll and shdocvw.dll have no DllInstall in this PE at all and
		// the text form answers 127 for both.
		return (hr == (HRESULT) ERROR_MOD_NOT_FOUND) ? S_OK : hr;
	}
	return (HRESULT) lResult;
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
