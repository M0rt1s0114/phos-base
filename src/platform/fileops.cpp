// The file, shortcut, process-ending and DLL capabilities.
//
// Written from the behaviour of the reference implementation as observed from
// the outside, rather than from its source.
//
// Already ours before this slice, and deliberately left alone: FsDelete,
// DirCreate, FsReadAll, FsWriteAll, FsRename, FsHardLink, FsTempDir,
// CreateShortcut, KillProc and NativeCall were written first; rewriting them
// again would only add risk.

// A path that is about to be deleted is checked before anything touches it.
//
// The shape this is here for is "a variable that expanded to nothing":
// "%&dir%\*.*" with dir unset is "\*.*", which SHFileOperation reads as
// "everything in the root of the current drive". An empty path and a bare
// "X:\" are refused for the same reason. A path that names a directory and then
// a wildcard inside it is fine -- that is a normal cleanup.
HRESULT ValidateDestructivePath(PCTSTR ptzPath)
{
	TCHAR tzFull[MAX_PATH];
	DWORD dwLen;
	PTSTR p;

	if (!ptzPath || !ptzPath[0])
	{
		return E_INVALIDARG;
	}

	// Decide about the path that will be acted on, not about how it was
	// spelled: "." , ".." and relative paths all have to be resolved first.
	dwLen = GetFullPathName(ptzPath, _NumOf(tzFull), tzFull, NULL);
	if ((dwLen == 0) || (dwLen >= _NumOf(tzFull)))
	{
		return E_INVALIDARG;
	}

	p = tzFull;
	if ((p[0] == '\\') && (p[1] == '\\'))
	{
		// "\\server\share" -- skip the four leading separators.
		UINT uSeen = 0;

		while (*p && (uSeen < 4))
		{
			if (*p == '\\')
			{
				uSeen++;
			}
			p++;
		}
	}
	else
	{
		p += 2;			// "X:"
	}
	while (*p == '\\')
	{
		p++;
	}

	// Nothing left: the path was the drive or share root itself.
	if (!*p)
	{
		return E_INVALIDARG;
	}

	// A wildcard where the first component should be: "X:\*.*".
	if ((*p == '*') || (*p == '?'))
	{
		return E_INVALIDARG;
	}

	return S_OK;
}

// "FILE <path>" deletes. "FILE <from>=><to>" copies and "FILE <from>-><to>"
// moves.
//
// Only the two-character operators count. A ">" that is not part of one is an
// error now: it used to end the source path and leave the operation as a
// delete, which turned a mistyped "copy a to b" into "delete a".
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// The file capabilities
//
// The operations themselves: delete, and copy/move. The language's fs.* verbs
// call these instead of building command strings.
//////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT FsDelete(PCTSTR ptzPath)
{
	SHFILEOPSTRUCT so = {0};
	std::vector<TCHAR> from;
	int iResult;
	HRESULT hr = ValidateDestructivePath(ptzPath);

	if (hr != S_OK)
	{
		return hr;
	}

	// SHFileOperation wants a double-NUL terminated list of names. The legacy
	// path got that by writing past the end of the string it was handed, which
	// only worked because that string was the script text itself.
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
	//
	// A directory that is already there is not a failure: the caller asked for
	// this path to exist and it does. Answering ERROR_ALREADY_EXISTS made every
	// caller test for existence first, and the test is exactly what the second
	// call was for. A *file* of that name is still refused -- being told "that
	// worked" about a path that is not a directory is worse than the error.
	UDirCreate((PTSTR) ptzPath);
	if (CreateDirectory(ptzPath, NULL))
	{
		return S_OK;
	}
	{
		DWORD dwErr = GetLastError();

		if (dwErr == ERROR_ALREADY_EXISTS)
		{
			DWORD dwAttr = GetFileAttributes(ptzPath);

			if ((dwAttr != INVALID_FILE_ATTRIBUTES) && (dwAttr & FILE_ATTRIBUTE_DIRECTORY))
			{
				return S_OK;
			}
		}
		return (HRESULT) dwErr;
	}
}

// The size of a file, or of everything under a directory.
//
// `fs.stat` is the caller, and a directory's size is the sum of what is under it
// rather than the size of the directory entry. A directory that is a reparse
// point -- a junction, a mount point -- is not followed: it can point back at a
// directory already being walked, and the walk would then never end.
//
// FALSE means "this could not be measured" and *pdwError says why. That is not
// the same answer as zero, and the caller turns it into an error rather than a
// size.
BOOL PathSizeBytes(PCTSTR ptzPath, UINT64 *puSize, DWORD *pdwError)
{
	WIN32_FILE_ATTRIBUTE_DATA fa;
	WIN32_FIND_DATA fd;
	HANDLE hFind;
	TCHAR tzPattern[MAX_PATH];
	UINT64 uTotal = 0;

	if (!GetFileAttributesEx(ptzPath, GetFileExInfoStandard, &fa))
	{
		*pdwError = GetLastError();
		return FALSE;
	}
	if (!(fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
	{
		*puSize = ((UINT64) fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
		return TRUE;
	}

	// A path that cannot fit is not measured: the caller named something this
	// buffer cannot spell.
	if ((UStrLen(ptzPath) + 3) >= _NumOf(tzPattern))
	{
		*pdwError = ERROR_FILENAME_EXCED_RANGE;
		return FALSE;
	}
	UStrPrint(tzPattern, TEXT("%s\\*"), ptzPath);
	hFind = FindFirstFile(tzPattern, &fd);
	if (hFind == INVALID_HANDLE_VALUE)
	{
		// An empty directory answers ERROR_FILE_NOT_FOUND -- that one really is
		// size zero. Anything else ("nobody may list this folder") means the
		// size is not knowable, which is a different kind of answer.
		DWORD dwErr = GetLastError();

		if (dwErr != ERROR_FILE_NOT_FOUND)
		{
			*pdwError = dwErr;
			return FALSE;
		}
		*puSize = 0;
		return TRUE;                   // an empty directory is size zero
	}
	do
	{
		TCHAR tzChild[MAX_PATH];

		if ((UStrCmp(fd.cFileName, TEXT(".")) == 0) || (UStrCmp(fd.cFileName, TEXT("..")) == 0))
		{
			continue;
		}
		if ((UStrLen(ptzPath) + UStrLen(fd.cFileName) + 2) >= _NumOf(tzChild))
		{
			continue;		// the same: a name that does not fit is not that file
		}
		UStrPrint(tzChild, TEXT("%s\\%s"), ptzPath, fd.cFileName);
		if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
			(!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)))
		{
			UINT64 uChild = 0;

			if (!PathSizeBytes(tzChild, &uChild, pdwError))
			{
				FindClose(hFind);
				return FALSE;
			}
			uTotal += uChild;
		}
		else
		{
			uTotal += ((UINT64) fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
		}
	}
	while (FindNextFile(hFind, &fd));
	FindClose(hFind);

	*puSize = uTotal;
	return TRUE;
}

// The rest of the file capability: reading, writing, and the two operations the
// legacy commands spell with flags. The legacy READ/WRIT are a line-oriented,
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
	// A short write is a write that did not finish. The handle is not
	// overlapped, so this should not happen -- and a file reported as saved
	// while its tail is missing is exactly the answer this project refuses.
	if (cbWritten != cbData)
	{
		CloseHandle(hFile);
		return (HRESULT) ERROR_WRITE_FAULT;
	}
	CloseHandle(hFile);
	return S_OK;
}

HRESULT FsRename(PCTSTR ptzFrom, PCTSTR ptzTo)
{
	// COPY_ALLOWED so a move lands even when the two names are on volumes that
	// cannot be renamed across, which is what the legacy command does.
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
//
// The shortcut capability.
//
// The legacy form packs everything into one comma-separated string and mutates
// it in place; the structured form takes the pieces as arguments, which is the
// point -- no comma, no quoting rule, and no buffer to write past the end of.
//
// bVerbatimTarget keeps the legacy '*' prefix working: without it the target is
// resolved the way Explorer would resolve it. The structured verbs never ask
// for it, because a caller that wants a literal path can pass one that exists.
HRESULT CreateShortcut(PCTSTR ptzLnk, PCTSTR ptzTarget, PCTSTR ptzArgs, PCTSTR ptzIcon,
	INT iIcon, BOOL bMinimized, BOOL bVerbatimTarget)
{
	IShellLink *pLink;
	HRESULT hResult;
	TCHAR tzTarget[MAX_PATH];
	TCHAR tzIcon[MAX_PATH];

	// Both of these are copied into a MAX_PATH buffer further down, and a
	// shortcut whose target or name was cut short would point somewhere else.
	if ((UStrLen(ptzTarget) >= MAX_PATH) || ((UStrLen(ptzLnk) + 4) >= MAX_PATH))
	{
		return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
	}

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

	HRESULT hInit = CoInitialize(NULL);
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

			// Each of these setters can refuse, and the answer that matters is
			// the first refusal: a shortcut whose path or icon did not take
			// would still be saved, and Save's S_OK used to overwrite the
			// failure, so the verb reported a shortcut that pointed somewhere
			// the script had not asked for. The chain stops at the first one.
			if (bMinimized)
			{
				hResult = pLink->SetShowCmd(SW_SHOWMINIMIZED);
			}
			if (SUCCEEDED(hResult))
			{
				hResult = pLink->SetPath((PTSTR) ptzTarget);
			}
			// A NULL argument or icon means "leave it out": the shell's setters
			// do not take NULL, and the analyser is right to say so.
			if (SUCCEEDED(hResult) && ptzArgs)
			{
				hResult = pLink->SetArguments((PTSTR) ptzArgs);
			}
			if (SUCCEEDED(hResult) && ptzIcon)
			{
				hResult = pLink->SetIconLocation((PTSTR) ptzIcon, iIcon);
			}

			// "Start in the target's folder" -- but only when the target has
			// one. UDirSplitPath truncates in place, hence the copy.
			if (SUCCEEDED(hResult))
			{
				UStrCopy(tzWork, (PTSTR) ptzTarget);
				if (UDirSplitPath(tzWork) != tzWork)
				{
					hResult = pLink->SetWorkingDirectory(tzWork);
				}
			}

			if (SUCCEEDED(hResult))
			{
				UStrPrint(tzLink, TEXT("%s.LNK"), ptzLnk);
				UStrToWStr(wzLink, tzLink, MAX_PATH);
				UDirCreate(tzLink);
				hResult = pFile->Save(wzLink, FALSE);
			}

			pFile->Release();
		}
		pLink->Release();
	}
	if (SUCCEEDED(hInit))
	{
		CoUninitialize();
	}
	return hResult;
}

// Parse Shortcut,Target,Param,IconPath,IconIndex
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// Find process
//
// With a name: a process whose image name matches. With no name: this
// process's parent. Zero when there is no answer.
//
// bExact separates the two kinds of caller. A person typing a name at a prompt
// matches a prefix, and that is what has always meant -- "explorer" stands for
// "explorer.exe". A script asking whether a program is running wants the program
// it named: "ex" must not answer yes because explorer.exe exists.
//
// pSkip lists process ids this call has already handled. KillProc needs it,
// and it is not an optimisation: a process that cannot be opened or cannot be
// terminated stays in the snapshot, so "find one, kill it, look again" would
// come back to the same id for ever, at 100% CPU, with the boot chain waiting
// behind it.
DWORD FindProcEx(PCTSTR ptzCmd, BOOL bExact, const std::vector<DWORD> *pSkip)
{
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
			if (!ptzCmd)
			{
				if (pe.th32ProcessID == dwSelf)
				{
					dwFound = pe.th32ParentProcessID;
					break;
				}
				continue;
			}

			if (pSkip)
			{
				BOOL bHandled = FALSE;

				for (size_t i = 0; i < pSkip->size(); i++)
				{
					if ((*pSkip)[i] == pe.th32ProcessID)
					{
						bHandled = TRUE;
						break;
					}
				}
				if (bHandled)
				{
					continue;
				}
			}

			if (bExact ? !UStrCmpI(pe.szExeFile, ptzCmd)
				: !UStrCmpNI(pe.szExeFile, ptzCmd, UStrLen(ptzCmd)))
			{
				dwFound = pe.th32ProcessID;
				break;
			}
		}
	}
	CloseHandle(hSnap);
	return dwFound;
}

DWORD FindProc(PCTSTR ptzCmd)
{
	return FindProcEx(ptzCmd, FALSE, NULL);
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// The process-ending capability. Every process whose image name matches goes;
// the answer is S_OK when at least one was terminated and S_FALSE when none
// was found. S_FALSE rather than an error is deliberate: the boot chain is
// allowed to kill something that is not running, and the trace records it.
HRESULT KillProc(PCTSTR ptzName, BOOL bExact)
{
	HRESULT hResult = S_FALSE;
	std::vector<DWORD> vHandled;

	if (!ptzName)
	{
		return S_OK;
	}

	// Each id is tried once. A process that refuses to die -- protected, or
	// owned by someone else -- is reported as "not killed" instead of being
	// retried until the machine is switched off.
	for (;;)
	{
		DWORD ProcessID = FindProcEx((PTSTR) ptzName, bExact, &vHandled);

		if (!ProcessID)
		{
			break;
		}
		vHandled.push_back(ProcessID);

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

	return hResult;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT Boot(BOOL bReboot)
{
	// Tell everyone first, the way the reference does, then ask the kernel. The
	// notifications are best effort: nothing reads what came back.
	DWORD_PTR dwResult;
	typedef DWORD (NTAPI *PNtShutdownSystem)(DWORD dwAction);
	PNtShutdownSystem NtShutdownSystem;

	SendMessageTimeout(HWND_BROADCAST, WM_QUERYENDSESSION, 0, 0, 0, 2000, &dwResult);
	SendMessageTimeout(HWND_BROADCAST, WM_ENDSESSION, 0, 0, 0, 2000, &dwResult);
	SendMessageTimeout(HWND_BROADCAST, WM_DESTROY, 0, 0, 0, 2000, &dwResult);

	// NtShutdownSystem is in no import library, so it is looked up by name.
	{
		HMODULE hNtdll = GetModuleHandle(TEXT("NTDLL"));

		NtShutdownSystem = hNtdll ? (PNtShutdownSystem) GetProcAddress(hNtdll, "NtShutdownSystem") : NULL;
	}
	if (!NtShutdownSystem)
	{
		return E_FAIL;
	}

	Priv(SE_SHUTDOWN_NAME);

	// 1 reboots, 2 shuts down. That mapping is the reference's, and the tracer
	// cannot see it: this call ends the run.
	return NtShutdownSystem(bReboot ? 1: 2);
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// Shutdown
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
// and "the function answered 126" are different things, and the reference's
// CALL $ deliberately reports the first one as success.
// Does this library name carry a path? If it does, the caller said where to
// look and nothing here second-guesses it.
BOOL IsLibraryPath(PCTSTR ptzLibrary)
{
	return (UStrChr(ptzLibrary, '\\') != NULL) || (UStrChr(ptzLibrary, '/') != NULL) ||
		(UStrChr(ptzLibrary, ':') != NULL);
}

// Where a bare library name is looked for: the system directory, and nowhere
// else.
//
// LoadLibrary's own order starts with the current directory, and in a PE the
// current directory is the folder the running boot script was read from -- the
// removable media the machine booted from. A planted copy of a DLL a boot
// script names by bare name would otherwise be loaded with the boot driver's
// rights. Written as a function because there is more than one caller:
// NativeCall below and WIMGAPI in volume.cpp.
BOOL SystemLibraryPath(PCTSTR ptzLibrary, PTSTR ptzOut, UINT cchOut)
{
	if ((GetSystemDirectory(ptzOut, cchOut) == 0) ||
		((UStrLen(ptzOut) + UStrLen(ptzLibrary) + 2) > cchOut))
	{
		return FALSE;
	}
	return SUCCEEDED(StringCchCat(ptzOut, cchOut, TEXT("\\"))) &&
		SUCCEEDED(StringCchCat(ptzOut, cchOut, ptzLibrary));
}

HRESULT NativeCall(PCTSTR ptzLibrary, PCTSTR ptzProc, PVOID *ppvArgs, UINT uArgs, LONG *plResult)
{
	HMODULE hLib;
	CHAR szProc[MAX_NAME];
	FARPROC f;
	TCHAR tzPath[MAX_PATH];

	*plResult = 0;

	// Resolve the library before loading it, so the search order is ours and not
	// the loader's (see SystemLibraryPath above for why that matters).
	if (!IsLibraryPath(ptzLibrary))
	{
		if (!SystemLibraryPath(ptzLibrary, tzPath, _NumOf(tzPath)))
		{
			return (HRESULT) ERROR_MOD_NOT_FOUND;
		}
		ptzLibrary = tzPath;
	}

	// Dependencies of a library that was found are looked for beside it rather
	// than in the current directory, so the drive the script came from cannot
	// supply one either. For a relative path the flag has no effect, which is the
	// caller's own choice to have made.
	hLib = LoadLibraryEx(ptzLibrary, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!hLib)
	{
		return (HRESULT) ERROR_MOD_NOT_FOUND;
	}
	if (!ptzProc)
	{
		ptzProc = TEXT("DllRegisterServer");
	}
	// CALL $dll,* is preload only: the reference keeps the module loaded so a
	// later call is cheaper, so this path deliberately does not free it.
	if (!UStrCmp(ptzProc, TEXT("*")))
	{
		return S_OK;
	}
	UStrToAStr(szProc, (PTSTR) ptzProc, MAX_NAME);
	f = GetProcAddress(hLib, szProc);
	if (!f)
	{
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
		return E_INVALIDARG;
	}

	// The library stays loaded, like the preload path above. Unloading it the
	// moment the call returns is a use-after-unload waiting to happen: an export
	// that starts a thread, registers a callback or hands out a pointer into its
	// own code would be referring to a module that is gone. One process-wide
	// load per distinct library is a cost a boot config can pay.
	return S_OK;
}

//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
