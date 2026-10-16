// The process capability: EXEC, INIT, and the shell plumbing that hangs off
// them.
//
// Written from the behaviour of the reference implementation as observed from
// the outside, rather than from its source. The comments that were here before
// described the reference's internals; these describe the contract we have to
// hold.

// Publishing the letters this machine's CD-ROM and removable volumes have.
// Windows knows; a script inside a PE does not, so they go into the environment
// where an expansion can reach them, under the reference's own names:
//
//   CDROM=D:  CDROM1=D:  CDROM2=E:  CDROM0=<count>
//
// and the same with USB for removable volumes. Scripts in the wild read those
// names, so they stay. Returns how many drives of that kind were found.
//
// Note: the reference's INIT option "U" also promises to watch for a stick
// being plugged in. This publishes what is already there.
// Wait for one handle, and keep the message pump alive while waiting.
//
// This matters because the chain runs in the process that owns the tray window.
// A plain WaitForSingleObject stops that thread from dispatching anything for
// the length of the wait, and a child that broadcasts to the session's top-level
// windows -- AddFontResource does it, and so does anything that changes a system
// setting -- blocks on this window until the wait is over. The wait would then
// last until its timeout, every time, and a timeout of none would last for ever.
//
// The slice is short enough that a queued message is not held up noticeably, and
// the caller's timeout is still the caller's: the slices add up to it.
DWORD WaitPumping(HANDLE h, DWORD dwTimeout)
{
	DWORD dwElapsed = 0;

	for (;;)
	{
		DWORD dwSlice = 100;
		DWORD dwWait;

		if (dwTimeout != INFINITE)
		{
			if (dwElapsed >= dwTimeout)
			{
				return WAIT_TIMEOUT;
			}
			if ((dwTimeout - dwElapsed) < dwSlice)
			{
				dwSlice = dwTimeout - dwElapsed;
			}
		}
		// QS_ALLINPUT: posted, sent, timer, paint and input all wake it, which is
		// what a window's thread has to answer.
		dwWait = MsgWaitForMultipleObjects(1, &h, FALSE, dwSlice, QS_ALLINPUT);
		if (dwWait == WAIT_OBJECT_0)
		{
			return WAIT_OBJECT_0;
		}
		if (dwWait == (WAIT_OBJECT_0 + 1))
		{
			MSG msg;

			while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
		}
		if (dwTimeout != INFINITE)
		{
			dwElapsed += dwSlice;
		}
	}
}

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
	TCHAR tzRoots[MAX_PATH] = {0};	// four characters per drive letter, twenty-six letters
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
// The reference patched the child process: it wrote a hand-encoded 32-bit stub
// over that child's ExitWindowsEx so that "the user asked Windows to shut down"
// would instead run a fixed command line -- the stub carries the old program
// name with it. Two things about it:
//
//   * the x64 build never had it either; HookProcess has always been a no-op
//     there;
//   * nothing uses it. Not the startup chain, not a case, not the shipped
//     disposition.
//
// So it is a no-op on both architectures now. Re-deriving somebody else's
// machine code would make it ours in form only, and there is nothing left that
// exercises it. "$" reaches exactly the code path the x64 build has always run.
VOID HookProcess(HANDLE hProcess)
{
	UNREFERENCED_PARAMETER(hProcess);
}

// The keyboard hook's handle, read or written from two threads: the window
// thread installs and removes it, and the shell thread (StartShell) asks whether
// one is already in. Both sides use the interlocked forms -- a plain pointer
// would be atomic in practice on the machines this runs on, but "in practice" is
// not something the next reader should have to establish.
static HHOOK ReadHookHandle(VOID)
{
	return (HHOOK) InterlockedCompareExchangePointer(
		(PVOID volatile *) &g_hHook, NULL, NULL);
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////


////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Start a program.
//
// Two things in this process start one: the keyboard hook below, which wants the
// task manager up the moment Ctrl+Alt+Delete is pressed, and MainThread putting
// the shell back after it went away. Neither reads a result.
//
// What used to be here was the text EXEC: its modifier run ('!', '@', '=', '$',
// '&'), its "-hide" and "-wd:" options, and a branch that wrote a command line
// into the Run key instead of running it. `proc.run` and `boot.add_run` own those
// jobs in the language now, so what is left is the launch itself.
//
// The command line goes to CreateProcess as it stands: a quoted
// "X:\Program Files\..." arrives whole instead of being trimmed.
VOID StartProgram(PCTSTR ptzCmd)
{
	STARTUPINFO si = {0};
	PROCESS_INFORMATION pi;
	TCHAR tzCmd[MAX_STR];

	if (!UStrCopyTo(tzCmd, ptzCmd))
	{
		return;			// a line that cannot fit is not run
	}

	si.cb = sizeof(si);
	si.lpDesktop = TEXT("WinSta0\\Default");
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_NORMAL;

	if (CreateProcess(NULL, tzCmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
	{
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	}

	// WinPE runs out of memory long before it runs out of anything else, and the
	// child just took a copy of our working set.
	SetProcessWorkingSetSize(CurrentProcess, (SIZE_T) -1, (SIZE_T) -1);
}

// Start the PE's shell and hand it to the tray.
//
// The old form spelled this "$<path>": start it, give it a moment, hook the
// child, and tell the tray window to take the keyboard hook and put the icon up.
// That whole sequence is this function now -- there is no modifier to spell and
// no text to parse. The 1.5 seconds is the flat wait the text path had.
HRESULT StartShell(PCTSTR ptzShell)
{
	STARTUPINFO si = {0};
	PROCESS_INFORMATION pi;
	TCHAR tzCmd[MAX_STR];

	if (!UStrCopyTo(tzCmd, ptzShell))
	{
		return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
	}

	si.cb = sizeof(si);
	si.lpDesktop = TEXT("WinSta0\\Default");
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_NORMAL;

	if (!CreateProcess(NULL, tzCmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
	{
		return (HRESULT) GetLastError();
	}

	Sleep(1500);
	HookProcess(pi.hProcess);
	if (!ReadHookHandle())
	{
		SendMessage(MainWindowHwnd, FindShellMsg, 1, 0);
	}
	if (TrayIcon)
	{
		SendMessage(MainWindowHwnd, FindShellMsg, 2, 0);
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	SetProcessWorkingSetSize(CurrentProcess, (SIZE_T) -1, (SIZE_T) -1);
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
		StartProgram(TASKMANAGER);
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
		if (!ReadHookHandle())
		{
			InterlockedExchangePointer((PVOID volatile *) &g_hHook,
				SetWindowsHookEx(WH_KEYBOARD_LL, InitProc, g_hInst, 0));
		}
		return;
	}

	if (ReadHookHandle())
	{
		UnhookWindowsHookEx(g_hHook);
		// The flag the tray reads back, either way.
		InterlockedExchangePointer((PVOID volatile *) &g_hHook, NULL);
	}
}
//////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
#define pzStrDLLInstall TEXT(".DLL,DllInstall,#1,I")
// INIT <flags> -- the shell's setup steps, plus the two marks the tray reads back
// later.
//
// The flags are substrings, not a parsed list: "C", "I", "K" and "U" are looked
// for anywhere in the argument and in any case, which is how the reference looked
// for them. None of these steps is INIT's answer: the DLL calls below can fail on
// an image with no DllInstall at all and INIT still answers 0.
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
// The shell handoff, as recorded under HKLM\SOFTWARE\Phosphor -- this program's
// own key and not an upstream one. The *value names* are fixed, because other
// parts of the program read them; the key itself is ours to name.
#define PESHELL TEXT("Shell")
void GetShell(bool DeleteSettingKey)
{		
	if (DeleteSettingKey) 
	{		
		SHSetValue(HKEY_LOCAL_MACHINE, PHOS_STATE,TEXT("SysShell"), REG_SZ, MyShell,
			(UStrLen(MyShell) + 1) * sizeof(TCHAR));
		SHDeleteValue(HKEY_LOCAL_MACHINE, PHOS_STATE,PESHELL);
	}
	else
	{	
		BOOL bInitProc = FALSE;
		DWORD dwType=REG_SZ;
		DWORD dwSize=MAX_PATH;

		SHGetValue(HKEY_LOCAL_MACHINE,PHOS_STATE, PESHELL,&dwType, &MyShell[0], &dwSize);

		// Each value is read into a buffer of its own size. The reference kept
		// the string's size in dwSize for all three, which for the two DWORDs
		// meant telling the registry it may write up to MAX_PATH bytes into a
		// four-byte BOOL.
		dwType=REG_DWORD;
		dwSize=sizeof(TrayIcon);
		SHGetValue(HKEY_LOCAL_MACHINE, PHOS_STATE, TEXT("TrayIcon"), &dwType, &TrayIcon, &dwSize);
		dwSize=sizeof(bInitProc);
		SHGetValue(HKEY_LOCAL_MACHINE, PHOS_STATE, TEXT("InitProc"), &dwType, &bInitProc, &dwSize);

		if ((!g_hHook) && (bInitProc) && (MainWindowHwnd))
		{
			SendMessage(MainWindowHwnd, FindShellMsg, 1, 0);
		}
	}
}
//////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
