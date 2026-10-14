// The process capability: EXEC, INIT, and the shell plumbing that hangs off them.

// Publishing the letters this machine's CD-ROM and removable volumes have.
// Windows knows; a script inside a PE does not, so they go into the environment
// where an expansion can reach them, under the names it publishes:
//
//   CDROM=D:  CDROM1=D:  CDROM2=E:  CDROM0=<count>
//
// and the same with USB for removable volumes. Scripts in the wild read those
// names, so they stay. Returns how many drives of that kind were found.
//
// Note: the text form's INIT option "U" also promises to watch for a stick
// being plugged in. This publishes what is already there.
UINT InitDriveList(UINT pzDriveType)
{
	PCTSTR ptzPrefix = NULL;

	if (pzDriveType == DRIVE_CDROM)
	{
		ptzPrefix = TEXT("CDROM");
	}
	else if (pzDriveType == DRIVE_REMOVABLE)
	{
		ptzPrefix = TEXT("USB");
	}
	if (!ptzPrefix)
	{
		return 0;
	}

	// GetLogicalDriveStrings hands back a double-NUL terminated run of "X:\"
	// roots. Walk it by length, which is the walking rule the list itself uses.
	TCHAR tzRoots[MAX_NAME] = {0};
	if (!GetLogicalDriveStrings(_NumOf(tzRoots) - 1, tzRoots))
	{
		return 0;
	}

	UINT uFound = 0;
	TCHAR tzPair[MAX_NAME];

	for (PTSTR ptzRoot = tzRoots; *ptzRoot; ptzRoot += UStrLen(ptzRoot) + 1)
	{
		if (MyGetDriveType(ptzRoot) != pzDriveType)
		{
			continue;
		}

		TCHAR tzLetter[3] = {ptzRoot[0], ':', 0};

		uFound++;
		UStrPrint(tzPair, TEXT("%s%d=%s"), ptzPrefix, uFound, tzLetter);
		SetEnvironment(tzPair, TRUE, FALSE);

		// The first one also gets the unnumbered name, which is the only
		// spelling older configs use.
		if (uFound == 1)
		{
			UStrPrint(tzPair, TEXT("%s=%s"), ptzPrefix, tzLetter);
			SetEnvironment(tzPair, TRUE, FALSE);
		}
	}

	if (uFound)
	{
		UStrPrint(tzPair, TEXT("%s0=%d"), ptzPrefix, uFound);
		SetEnvironment(tzPair, TRUE, FALSE);
	}

	return uFound;
}
//////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// EXEC's "$" modifier.
//
// The "$" modifier once patched the child process: it wrote a hand-encoded
// 32-bit stub over that child's ExitWindowsEx so that "the user asked Windows to
// shut down" would instead run a fixed command line. Two things about it:
//
//   * only the 32-bit build ever had it; HookProcess has always been a no-op in
//     the x64 build;
//   * nothing uses it. Not the startup chain, not a shipped configuration.
//
// So it is a no-op on both architectures: "$" reaches the same code path on
// either one.
VOID HookProcess(HANDLE hProcess)
{
	UNREFERENCED_PARAMETER(hProcess);
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////


////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Execute command
#define RunsKey TEXT("SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run")

// One "-xxx" option. The two the text form knows are the ones handled here; the
// rest are skipped, and that silence is deliberate - see the note at the loop.
static VOID ApplyExecOption(PCTSTR ptzFlag, STARTUPINFO *psi, PTSTR ptzWorkDir)
{
	if (!UStrCmpI(ptzFlag, TEXT("-hide")))
	{
		psi->wShowWindow = SW_HIDE;
	}
	else if (!UStrCmpNI(ptzFlag, TEXT("-wd:"), 4))
	{
		UStrCopy(ptzWorkDir, UStrTrim((PTSTR) (ptzFlag + 4)));
	}
}

HRESULT Exec(CoreContext& ctx, PTSTR ptzCmd)
{
	STARTUPINFO si = {0};
	PROCESS_INFORMATION pi = {0};

	si.cb = sizeof(si);
	si.lpDesktop = TEXT("WinSta0\\Default");
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_NORMAL;

	BOOL bWait = FALSE;
	BOOL bHook = FALSE;
	BOOL bRunKey = FALSE;
	TCHAR tzWorkDir[MAX_PATH];

	tzWorkDir[0] = 0;			// empty means "inherit the current directory"

	// The one-character modifiers. They are a run of leading characters, so
	// "EXEC !=prog" is wait plus hide, in either order.
	{
		BOOL bMoreMods = TRUE;

		while (bMoreMods)
		{
			switch (*ptzCmd)
			{
			case '!':                       // hidden
				si.wShowWindow = SW_HIDE;
				break;
			case '@':                       // the logon desktop
				si.lpDesktop = TEXT("WinSta0\\WinLogon");
				break;
			case '=':
				bWait = TRUE;
				break;
			case '$':
				bHook = TRUE;
				break;
			case '&':
				bRunKey = TRUE;
				break;
			default:
				bMoreMods = FALSE;
				break;
			}
			if (bMoreMods)
			{
				ptzCmd++;
			}
		}
	}

	// Named options, the form the text form writes as "EXEC -hide prog args": a run
	// of "-xxx" tokens in front of the command line. An option that is not
	// implemented is skipped, which is what the text form does - a typo is
	// therefore silent, and that is worth knowing before trusting one.
	while (*ptzCmd == '-')
	{
		PTSTR ptzNext = UStrChr(ptzCmd, ' ');
		PTSTR ptzEnd = ptzNext ? ptzNext : (ptzCmd + UStrLen(ptzCmd));
		TCHAR tzFlag[MAX_NAME];
		UINT uLen = (UINT) (ptzEnd - ptzCmd);

		if (uLen >= MAX_NAME)
		{
			uLen = MAX_NAME - 1;
		}
		UStrCopyN(tzFlag, ptzCmd, uLen + 1);
		ApplyExecOption(tzFlag, &si, tzWorkDir);

		if (!ptzNext)
		{
			ptzCmd = ptzEnd;
			break;
		}
		ptzCmd = UStrTrim(ptzNext + 1);
	}

	// "&" stores the command line instead of running it, under its last path
	// component. "=" picks the machine-wide list rather than the current user's,
	// which is the only thing "=" means here.
	if (bRunKey)
	{
		PTSTR ptzName = UStrRChr(ptzCmd, '\\');
		HKEY hRoot = bWait ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;

		ptzName = ptzName ? (ptzName + 1) : ptzCmd;
		return SHSetValue(hRoot, RunsKey, ptzName, REG_SZ, ptzCmd, UStrLen(ptzCmd) * sizeof(TCHAR));
	}

	// The command line goes to CreateProcess as it stands. That is the point of
	// routing the process capability around the text layer: a quoted
	// "X:\Program Files\..." arrives whole instead of being trimmed.
	BOOL bResult = CreateProcess(NULL, ptzCmd, NULL, NULL, FALSE, 0, NULL,
		tzWorkDir[0] ? tzWorkDir : NULL, &si, &pi);

	if (!bResult)
	{
		// The Win32 error from CreateProcess is handed back rather than a bare
		// failure, because scripts branch on it: a program that is not there is 2
		// (file not found), not 1.
		return (HRESULT) GetLastError();
	}

	// Both handles are ours from here on. The guard is so that the wait path and
	// the hook path cannot leave one behind.
	struct CloseProcessHandles
	{
		PROCESS_INFORMATION *p;
		~CloseProcessHandles()
		{
			CloseHandle(p->hThread);
			CloseHandle(p->hProcess);
		}
	} closeOnExit = {&pi};

	if (bHook)
	{
		Sleep(1500);
		HookProcess(pi.hProcess);
		if (!g_hHook)
		{
			SendMessage(MainWindowHwnd, FindShellMsg, 1, 0);
		}
		if (TrayIcon)
		{
			SendMessage(MainWindowHwnd, FindShellMsg, 2, 0);
		}
	}

	// WinPE runs out of memory long before it runs out of anything else, and the
	// child just took a copy of our working set. The text form trims after every
	// spawn; so do we.
	SetProcessWorkingSetSize(CurrentProcess, (SIZE_T) -1, (SIZE_T) -1);

	if (bWait)
	{
		WaitForSingleObject(pi.hProcess, INFINITE);
	}

	return S_OK;
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Ctrl+Alt+Del handler
TCHAR TASKMANAGER[] = TEXT("TaskMgr.exe");
// Is this the combination the hook is here for? Ctrl+Alt+Delete, and nothing else.
static BOOL IsTaskManagerKey(INT iCode, WPARAM wParam, LPARAM lParam)
{
	if ((iCode != HC_ACTION) || (wParam != WM_KEYDOWN))
	{
		return FALSE;
	}
	if (((LPKBDLLHOOKSTRUCT) lParam)->vkCode != VK_DELETE)
	{
		return FALSE;
	}
	return ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) &&
		((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
}

// The keyboard hook: Ctrl+Alt+Delete opens the task manager instead of going to
// the system.
LRESULT CALLBACK InitProc(INT iCode, WPARAM wParam, LPARAM lParam)
{
	if (IsTaskManagerKey(iCode, wParam, lParam))
	{
		Exec(g_ctx, TASKMANAGER);
		return TRUE;                    // handled: the system does not see it
	}
	return CallNextHookEx(g_hHook, iCode, wParam, lParam);
}
/////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
// The hook is installed on request (INIT K, or the tray menu) and removed on the
// way out. g_hHook is the flag both ways round: the window procedure checks it
// to decide whether the tray's keyboard entry is ticked.
VOID InstallHookKeyBoard(BOOL Install)
{
	if (Install)
	{
		if (!g_hHook)
		{
			g_hHook = SetWindowsHookEx(WH_KEYBOARD_LL, InitProc, g_hInst, 0);
		}
		return;
	}

	if (g_hHook)
	{
		UnhookWindowsHookEx(g_hHook);
		g_hHook = NULL;                 // the flag the tray reads back, either way
	}
}
//////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
#define pzStrDLLInstall TEXT(".DLL,DllInstall,#1,I")
// INIT <flags> -- the shell's setup steps, plus the two marks the tray reads back
// later.
//
// The flags are substrings, not a parsed list: "C", "I", "K" and "U" are looked
// for anywhere in the argument and in any case, which is how the text form looked
// for them. None of these steps is INIT's answer: the DLL calls below can fail on
// an image with no DllInstall at all, and INIT still answers 0.
HRESULT Init(CoreContext& ctx, PTSTR ptzCmd)
{
	TCHAR tzPath[MAX_NAME];

	// An empty name with the init flag set is how the built-in directory
	// variables (%Desktop%, %Programs%, %StartMenu%, ...) get established, and
	// they go into the *process* environment, not just our own variable table,
	// so a child process inherits them.
	UStrCopy(tzPath, TEXT(""));
	SetEnvironment(tzPath, FALSE, TRUE);

	// Three opportunistic shell hooks, their answers dropped on purpose: browseui
	// and shdocvw have no DllInstall in this PE at all - the text form answers
	// 127 there too - and adding the results up would report 0x8004, two
	// E_NOINTERFACE values summed into one HRESULT.
	{
		static PCTSTR c_ShellDlls[] = { TEXT("SHELL32"), TEXT("BROWSEUI"), TEXT("SHDOCVW") };

		for (UINT i = 0; i < _NumOf(c_ShellDlls); i++)
		{
			UStrPrint(tzPath, TEXT("%s%s"), c_ShellDlls[i], pzStrDLLInstall);
			CallDll(tzPath);
		}
	}

	// C publishes the CD-ROM letters, U the removable ones, and I and K are the
	// two marks the tray code reads back later: put the icon up, install the
	// keyboard hook.
	if (StrStrI(ptzCmd, TEXT("C")))
	{
		InitDriveList(DRIVE_CDROM);
	}
	if (StrStrI(ptzCmd, TEXT("I")))
	{
		BOOL bIcon = TRUE;

		SHSetValue(HKEY_LOCAL_MACHINE, PELOGON, TEXT("TrayIcon"), REG_DWORD, &bIcon, sizeof(bIcon));
	}
	if (StrStrI(ptzCmd, TEXT("K")))
	{
		BOOL bHook = TRUE;

		SHSetValue(HKEY_LOCAL_MACHINE, PELOGON, TEXT("InitProc"), REG_DWORD, &bHook, sizeof(bHook));
	}
	if (StrStrI(ptzCmd, TEXT("U")))
	{
		InitDriveList(DRIVE_REMOVABLE);
	}

	g_bInit = TRUE;
	return S_OK;
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
// The shell handoff, as recorded under HKLM\SOFTWARE\PELOGON. The key and its
// value names are read by scripts and by the tray code, so they are fixed.
#define PESHELL TEXT("Shell")
void GetShell(bool DeleteSettingKey)
{		
	if (DeleteSettingKey) 
	{		
		SHSetValue(HKEY_LOCAL_MACHINE, PELOGON,TEXT("SysShell"), REG_SZ, MyShell, UStrLen(MyShell) * sizeof(TCHAR));
		SHDeleteValue(HKEY_LOCAL_MACHINE, PELOGON,PESHELL);
	}
	else
	{	
		BOOL bInitProc = FALSE;
		DWORD dwType=REG_SZ;
		DWORD dwSize=MAX_PATH;

		SHGetValue(HKEY_LOCAL_MACHINE,PELOGON, PESHELL,&dwType, &MyShell[0], &dwSize);

		// Each value is read into a buffer of its own size. One size for all
		// three would tell the registry it may write up to MAX_PATH bytes for
		// the two DWORDs, whose buffers are four bytes.
		dwType=REG_DWORD;
		dwSize=sizeof(TrayIcon);
		SHGetValue(HKEY_LOCAL_MACHINE, PELOGON, TEXT("TrayIcon"), &dwType, &TrayIcon, &dwSize);
		dwSize=sizeof(bInitProc);
		SHGetValue(HKEY_LOCAL_MACHINE, PELOGON, TEXT("InitProc"), &dwType, &bInitProc, &dwSize);

		if ((!g_hHook) && (bInitProc) && (MainWindowHwnd))
		{
			SendMessage(MainWindowHwnd, FindShellMsg, 1, 0);
		}
	}
}
//////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
