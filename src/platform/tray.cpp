// The tray: its window, its icon, its menu and the hotkeys that reach them.
//
// The keyboard-binding text is "<word>#<virtual key>", where the words are looked
// for anywhere in the string and in any case. That is how HOTK and the IDS_Hotk
// default are written, so the search is a substring search rather than a parse.
static const struct
{
	PCTSTR	ptzWord;
	UINT	uMod;
} c_HotkeyWords[] =
{
	{ TEXT("ALT"),   MOD_ALT },
	{ TEXT("CTRL"),  MOD_CONTROL },
	{ TEXT("SHIFT"), MOD_SHIFT },
	{ TEXT("WIN"),   MOD_WIN },
};

void GetKeyModAndKeyCode(PTSTR ptzHotk, UINT &Key_Mod, UINT &Hotkey)
{
	for (UINT i = 0; i < _NumOf(c_HotkeyWords); i++)
	{
		if (StrStrI(ptzHotk, c_HotkeyWords[i].ptzWord))
		{
			Key_Mod = Key_Mod | c_HotkeyWords[i].uMod;
		}
	}

	{
		PTSTR ptzKey = UStrChr(ptzHotk, '#');

		if (ptzKey)
		{
			Hotkey = UStrToInt(ptzKey + 1);
		}
	}
}
//////////////////////////////////////////////////////////////////////////////////////////
// How the hotkey ids are laid out. 0 and 1 belong to the menu's own two (Ctrl+Alt+M
// and the IDS_Hotk default); the script's slots start at 2, and there are eight of
// them. WM_DESTROY clears the whole range, so the count of ids is not the count of
// slots.
#define HOTKEY_SLOTS		8
#define HOTKEY_ID_BASE		2
#define HOTKEY_ID_COUNT		10
//////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
// Declared here rather than in a header, the way the rest of this unity build
// shares prototypes: the dispatcher lives in engine/dispatch.cpp, which is
// included after this file.
HRESULT Command(PTSTR ptzCmd);
BOOL IsCommandLine(PTSTR ptzCmd);

// Run the text a hotkey or a tray menu item carries.
//
// That text can be a dialect command line ("kill explorer.exe", "LOAD ...") or a
// program command line ("notepad.exe"), and a configuration uses both forms. The
// command table decides which it is, and the program line is the fallback, so
// both writings work.
static VOID RunBoundCommand(PTSTR ptzCmd)
{
	if (IsCommandLine(ptzCmd))
	{
		Command(ptzCmd);
	}
	else
	{
		Exec(g_ctx, ptzCmd);
	}
}
//////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
// The tray menu's own hotkey: Ctrl+Alt+M, plus whatever IDS_Hotk names, written
// as "<mods>#<key>,<command>". One registration per process, hence bReg.
void RegMenuHotk(void)
{
	TCHAR tzHotk[MAX_NAME];
	PTSTR ptzCmd;

	if (bReg)
	{
		return;
	}

	RegisterHotKey(MainWindowHwnd, 0, MOD_ALT | MOD_CONTROL, 'M');

	LoadString(g_hInst, IDS_Hotk, tzHotk, _NumOf(tzHotk));
	ptzCmd = UStrChr(UStrTrim(tzHotk), ',');
	if (ptzCmd)
	{
		UINT uMod = 0;
		UINT uKey = 0;

		*ptzCmd++ = 0;
		UStrCopy(HotkCmd, ptzCmd);      // the command half, run when the key fires
		GetKeyModAndKeyCode(tzHotk, uMod, uKey);
		RegisterHotKey(MainWindowHwnd, 1, uMod, uKey);
	}
	bReg = TRUE;
}
//////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
// Pull "<mods>,<key>,<command>" apart in place. FALSE when the text does not have
// both commas, which is what a slot nobody wrote looks like.
static BOOL SplitHotkeyBinding(PTSTR ptzText, UINT *puMod, UINT *puKey)
{
	PTSTR ptzComma = UStrChr(ptzText, ',');

	if (!ptzComma)
	{
		return FALSE;
	}
	*ptzComma++ = 0;
	*puMod = UStrToInt(ptzComma);

	ptzComma = UStrChr(ptzComma, ',');
	if (!ptzComma)
	{
		return FALSE;
	}
	*ptzComma++ = 0;
	*puKey = UStrToInt(ptzComma);
	return TRUE;
}

// Register the hotkeys the tray knows about, reading them back out of PELOGON
// first: this runs again when the main thread sees the shell key appear, and a
// HOTK that was written after this file was first read has to be picked up.
//
// Eight slots, each holding "<mods>,<key>,<command>". The id is the slot plus two,
// so slots 0 and 1 stay with the menu's own two hotkeys.
//
// The read size is the real size of the buffer it reads into: MAX_STR against a
// MAX_PATH-wide array would be 1024 bytes for 520, and a long command line could
// run off the end of it.
void RegHotKey(void)
{
	for (UINT i = 0; i < HOTKEY_SLOTS; i++)
	{
		TCHAR tzValueName[MAX_NAME];
		DWORD dwType = REG_SZ;
		DWORD dwSize = sizeof(HotKeyCommand[i]);
		UINT uMod = 0;
		UINT uKey = 0;
		BOOL bRegistered;

		HotKeyCommand[i][0] = 0;
		UStrPrint(tzValueName, TEXT("HotKey%d"), i + 1);
		SHGetValue(HKEY_LOCAL_MACHINE, PELOGON, tzValueName, &dwType, &HotKeyCommand[i][0], &dwSize);

		if (!HotKeyCommand[i][0])
		{
			continue;                   // nobody bound this slot
		}
		if (!SplitHotkeyBinding(HotKeyCommand[i], &uMod, &uKey) || (uKey <= 0))
		{
			continue;                   // not a binding - there is no key to register
		}

		bRegistered = RegisterHotKey(MainWindowHwnd, i + HOTKEY_ID_BASE, uMod, uKey);

		// What happened goes back into the registry under a second name, so a boot
		// that cannot register a key says so somewhere a maintainer can read it.
		{
			TCHAR tzResult[MAX_STR];

			UStrPrint(tzValueName, TEXT("RegHotKey%d"), i + 1);
			UStrPrint(tzResult, TEXT("Result=%d; KeyMod=%d, HotKey=%d; CMD=%s"),
				bRegistered, uMod, uKey, HotKeyCommand[i]);
			SHSetValue(HKEY_LOCAL_MACHINE, PELOGON, tzValueName, REG_SZ, tzResult,
				UStrLen(tzResult) * sizeof(TCHAR));
		}
	}

	SHSetValue(HKEY_LOCAL_MACHINE, PELOGON, TEXT("MainWindowHwnd"), REG_DWORD,
		&MainWindowHwnd, sizeof(MainWindowHwnd));
}
//////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
// Entries the menu has room for, and the id the tray's own "hide" item carries.
#define MENU_COMMAND_MAX	128

// Store one item's command, and hand back the label the item should show.
//
// The text comes out of Resource.rc as "<label>,<command line>". The label stays on
// the item; the command is expanded now rather than when the item is clicked,
// because by then the environment it names may be different. It is parked in
// MenuCommand for the window procedure to hand to RunBoundCommand.
static PTSTR MenuItemLabel(PTSTR ptzText)
{
	PTSTR ptzCmd = UStrChr(ptzText, ',');

	if (ptzCmd)
	{
		TCHAR tzCmd[MAX_PATH];

		*ptzCmd++ = 0;
		ExpandEnvironmentStrings(UStrTrim(ptzCmd), tzCmd, _NumOf(tzCmd));
		UStrCopy(MenuCommand[ToalMenu], tzCmd);
		ToalMenu++;
	}
	return UStrTrim(ptzText);
}

// Walk a menu and split each item's text into a label and a command.
//
// Items are numbered from the first real id upwards, which is what maps a click
// back to its command -- MenuFirstID is that base. GetMenuItemID answers -1 for a
// submenu and 0 for a separator, so those two are not items. The item whose id is
// ID_HIDE_ICON is the tray's own entry and carries no command.
void SetMenuText(HMENU hsMenu)
{
	int nItems = GetMenuItemCount(hsMenu);

	for (int i = 0; i < nItems; i++)
	{
		int nId = GetMenuItemID(hsMenu, i);
		HMENU hSub;
		MENUITEMINFO mii;
		TCHAR tzText[MAX_STR];

		if (nId == -1)                  // a submenu: walk into it
		{
			hSub = GetSubMenu(hsMenu, i);
			if (hSub)
			{
				SetMenuText(hSub);
			}
			continue;
		}
		if (nId == 0)                   // a separator
		{
			continue;
		}
		if (!MenuFirstID)
		{
			MenuFirstID = nId;
		}
		if ((nId == ID_HIDE_ICON) || (ToalMenu >= MENU_COMMAND_MAX))
		{
			continue;                   // the tray's own entry, or the table is full
		}

		mii.cbSize = sizeof(mii);
		mii.fMask = MIIM_TYPE;
		mii.fType = MFT_STRING;
		mii.cch = _NumOf(tzText);
		mii.dwTypeData = tzText;
		GetMenuItemInfo(hsMenu, nId, FALSE, &mii);

		ModifyMenu(hsMenu, nId, MF_STRING, nId, MenuItemLabel(tzText));
	}
}
//////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
// Build the tray menu, or drop it.
//
// TRUE: load the resource once, register the hotkey that opens the menu, and walk
// the items through SetMenuText. FALSE: tear the submenu down and then the menu,
// and only clear a handle once its DestroyMenu has actually succeeded.
HMENU MyPopupMenu(BOOL Created)
{
	if (Created)
	{
		if (!hMenu)
		{
			hMenu = LoadMenu(g_hInst, MAKEINTRESOURCE(IDR_MENU1));
		}
		if (!hSubMenu && hMenu)
		{
			RegMenuHotk();
			ToalMenu = 0;
			hSubMenu = GetSubMenu(hMenu, 0);
			if (hSubMenu)
			{
				SetMenuText(hSubMenu);
			}
		}
		return hSubMenu;
	}

	if (hSubMenu && DestroyMenu(hSubMenu))
	{
		hSubMenu = NULL;
		if (hMenu && DestroyMenu(hMenu))
		{
			hMenu = NULL;
		}
	}
	return hSubMenu;
}
//////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
// Fill in the notice the shell needs, once. FpNotify.hWnd being set afterwards is
// what says the structure is ready; the tip is the string the resource carries
// (with the line MainThread appends when the build is not Chinese).
static void FillTrayNotice(void)
{
	HICON hIcon;

	FpNotify.cbSize = sizeof(FpNotify);
	FpNotify.hWnd = MainWindowHwnd;
	FpNotify.uID = 0;
	FpNotify.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	FpNotify.uCallbackMessage = CM_nTaskIcoMsg;
	UStrCopyN(FpNotify.szTip, MenuTipText, 64);

	hIcon = LoadIcon(g_hInst, (LPCTSTR) IDI_ICON1);
	if (!hIcon)
	{
		hIcon = LoadIcon(g_hInst, (LPCTSTR) IDI_Main);
	}
	FpNotify.hIcon = hIcon;
}

// Put the tray icon up, or take it down.
//
// Installing waits for explorer: Shell_NotifyIcon needs a shell to talk to, and
// during a boot there is a stretch of time before one exists. Nine attempts, a
// tenth of a second of alertable sleep before each, two tenths between them.
// SleepEx with TRUE is the alertable form -- that is what lets a pending APC run
// while this waits.
//
// Removing goes the other way round from installing: the icon first, the menu
// after, so the shell is never left holding a handle into a menu that is gone.
VOID InsTrayIcon(BOOL Install)
{
	if (Install)
	{
		for (UINT i = 0; i < 9; i++)
		{
			if (FindProc(EXPLORER))
			{
				SleepEx(100, TRUE);
				if (!hSubMenu)
				{
					hSubMenu = MyPopupMenu(TRUE);
					if (MainWindowHwnd && hSubMenu && !FpNotify.hWnd)
					{
						FillTrayNotice();
					}
				}
				if (FpNotify.hWnd && Shell_NotifyIcon(NIM_ADD, &FpNotify))
				{
					break;
				}
			}
			SleepEx(200, TRUE);
		}
	}
	else if (hSubMenu)
	{
		Shell_NotifyIcon(NIM_DELETE, &FpNotify);
		hSubMenu = MyPopupMenu(FALSE);
	}

	SetProcessWorkingSetSize(CurrentProcess, (SIZE_T) -1, (SIZE_T) -1);
}
//////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
// Show the menu where the cursor is. Bringing our own window forward first is what
// makes the popup close when the click lands somewhere else; the WM_NULL afterwards
// is the documented way of handing the foreground back.
void PopupLogonMenu(HWND hWND)
{
	POINT ptCursor;

	GetCursorPos(&ptCursor);
	SetForegroundWindow(MainWindowHwnd);
	TrackPopupMenu(hSubMenu, TPM_LEFTBUTTON, ptCursor.x, ptCursor.y, 0, hWND, NULL);
	PostMessage(hWND, WM_NULL, 0, 0);   // hand the foreground back
	SetProcessWorkingSetSize(CurrentProcess, (SIZE_T) -1, (SIZE_T) -1);
}
//////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
// WM_HOTKEY. 0 opens or closes the tray menu, 1 is the resource-bound one, and the
// rest are the script's slots -- the id is the slot plus the base they were
// registered under.
static void OnHotKey(WPARAM wParam)
{
	switch (LOWORD(wParam))
	{
	case 0:
		InsTrayIcon(!hSubMenu);
		break;
	case 1:
		RunBoundCommand(HotkCmd);
		break;
	default:
		RunBoundCommand(HotKeyCommand[wParam - HOTKEY_ID_BASE]);
	}
}

// FindShellMsg: the three steps the shell-detection thread asks for, in the order
// it asks for them -- re-read the bindings, put the keyboard hook in, put the icon
// up.
static void OnFindShell(WPARAM wParam)
{
	switch (wParam)
	{
	case 0:
		RegHotKey();
		GetShell(true);
		break;
	case 1:
		InstallHookKeyBoard(TRUE);
		break;
	case 2:
		InsTrayIcon(TRUE);
		break;
	}
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_DESTROY:
		for (UINT i = 0; i < HOTKEY_ID_COUNT; i++)
		{
			UnregisterHotKey(MainWindowHwnd, i);
		}
		PostQuitMessage(0);
		return 0;

	case WM_COMMAND:
		if (wParam == ID_HIDE_ICON)
		{
			InsTrayIcon(FALSE);
		}
		else
		{
			RunBoundCommand(MenuCommand[wParam - MenuFirstID]);
		}
		return 0;

	case WM_HOTKEY:
		OnHotKey(wParam);
		return 0;

	case FindShellMsg:
		OnFindShell(wParam);
		return 0;

	case CM_nTaskIcoMsg:
		// Either button opens the same menu.
		if ((lParam == WM_LBUTTONUP) || (lParam == WM_RBUTTONUP))
		{
			PopupLogonMenu(hWnd);
		}
		return 0;
	}
	return DefWindowProc(hWnd, message, wParam, lParam);
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////


////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// The window class name and the window's title. The class name is externally
// visible -- FindWindow and GetClassName reach it -- so it carries our name, and
// both the creator below and the finder further down use this one macro.
#define szWindowName	TEXT("Phosphor Commander")
#define szWindowClass	TEXT("PhosphorTrayWnd")
//////////////////////////////////////////////////////////////////////////////////////////
HWND CreateMainWindow(HINSTANCE hInstance/*, int nCmdShow*/)
{
	WNDCLASSEX wcex;
	HWND hWindow = NULL;

	ZeroMemory(&wcex, sizeof(wcex));
	wcex.cbSize = sizeof(wcex);
	wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = WndProc;
	wcex.hInstance = hInstance;
	wcex.lpszClassName = szWindowClass;

	if (RegisterClassEx(&wcex))
	{
		// Disabled and minimised: this window is here to receive messages, not to
		// be seen. The tray icon is the end of it the user has.
		hWindow = CreateWindow(szWindowClass, szWindowName, WS_DISABLED | WS_MINIMIZE,
			CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, NULL, NULL, hInstance, NULL);
	}

	{
		TCHAR tzChineseTip[MAX_NAME];

		UINT16ToUStr(tzChineseTip, AppTrayText, _NumOf(AppTrayText));
		// UStrCat is wcscat: a NULL source is a crash, not a no-op, so the flag is
		// tested here rather than folded into the pointer.
		if (!NotChineseLangID)
		{
			UStrCat(MenuTipText, tzChineseTip);
		}
	}

	return hWindow;
}
//////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
// Progress marker for the shell-locking thread. It has no log channel - it runs
// in the boot driver, before any script opens one - and "did it run, what did it
// see, did it decide the shell was already there" is the whole reason a shell
// that dies after the fact never comes back. The value stays in the registry to
// be read afterwards.
static VOID ThreadMark(PCTSTR ptzText)
{
	SHSetValue(HKEY_LOCAL_MACHINE, PELOGON, TEXT("MainThread"), REG_SZ,
		ptzText, UStrLen(ptzText) * sizeof(TCHAR));
}

// Watch for the shell this PE is supposed to end up with, and start it again if it
// goes away. Runs as a thread of its own, started from Main.
HRESULT MainThread(void)
{
	TCHAR tzMark[MAX_STR];
	TCHAR tzRunShell[MAX_PATH];
	TCHAR tzFound[MAX_NAME];
	PTSTR ptzShellName;

	ThreadMark(TEXT("enter: watching for the shell key"));
	while (!MyShell[0])
	{
		GetShell(false);
		Sleep(50);
	}

	// What the thread found, for whoever reads the marker afterwards.
	ptzShellName = UPathSplitName(MyShell);
	UStrPrint(tzRunShell, TEXT("=$%s"), MyShell);
	UStrPrint(tzFound, TEXT("%u"), (UINT) FindProc(ptzShellName));
	UStrPrint(tzMark, TEXT("shell=%s findproc=%s window=%u check=%u"),
		MyShell, tzFound, MainWindowHwnd ? 1 : 0, 1);
	ThreadMark(tzMark);

	if (MainWindowHwnd && !FindProc(ptzShellName))
	{
		TCHAR tzTip[MAX_NAME];

		LoadString(g_hInst, IDS_TrayTip, tzTip, _NumOf(tzTip));
		if (NotChineseLangID)
		{
			UStrCat(MenuTipText, tzTip);
		}
		UStrTrim(MenuTipText);
		SendMessage(MainWindowHwnd, FindShellMsg, 0, 0);
	}

	// The shell is expected to stay up from here; if it goes away again, start it
	// again.
	while (MyShell[0] && !FindProc(ptzShellName))
	{
		Exec(g_ctx, tzRunShell);
		Sleep(200);
	}
	ThreadMark(TEXT("done: loop left, no more shell watching"));
	return S_OK;
}
//////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
// A hotkey binding has to take effect when it is written, not whenever the shell
// detection thread happens to look.
//
// RegHotKey() is called from exactly one place: the FindShellMsg 0 handler, which
// MainThread sends once, the first time it sees PELOGON\Shell appear. The boot
// chain starts the shell well before Post\Link.phos binds anything, so that read
// found an empty registry and every key stayed dead - F1/F6/F7/F8 did nothing.
//
// The chain runs in a child process (Main spawns "<exe> LOAD <script>"), so this
// process usually has no window of its own. Re-register here when it does, and
// otherwise ask whichever process owns the tray window to re-read the registry.
// WM_USER carries no pointers, so sending it across processes is safe.
static VOID NotifyHotkeysChanged(void)
{
	HWND hTray;

	if (MainWindowHwnd)
	{
		RegHotKey();
		return;
	}
	hTray = FindWindow(szWindowClass, NULL);
	if (hTray)
	{
		SendMessage(hTray, FindShellMsg, 0, 0);
	}
}
//////////////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////////////
// HOTK <mods>#<key>,<command> -- record one binding.
//
// The binding goes into PELOGON as "HotKey<n>=<command>,<mods>,<key>", which is
// exactly the shape RegHotKey reads back, and eight is the ceiling for the same
// reason there are eight slots. NotifyHotkeysChanged is what makes it take effect
// now rather than whenever the shell-detection thread happens to look.
//
// The answer is 0 on every path, including the ones that bind nothing: a slot that
// is full and a line with no comma answer the same as a binding that went in. That
// is the compatible shape, and the startup chain binds hotkeys on every boot, so
// the trace has to keep saying 0.
HRESULT Hotk(CoreContext& ctx, PTSTR ptzHotk)
{
	PTSTR ptzCommand;
	UINT uMod = 0;
	UINT uKey = 0;
	TCHAR tzValueName[16];
	TCHAR tzBinding[MAX_PATH];

	if (HotkCount >= HOTKEY_SLOTS)
	{
		return S_OK;
	}
	ptzCommand = UStrChr(ptzHotk, ',');
	if (!ptzCommand)
	{
		return S_OK;
	}

	*ptzCommand++ = 0;
	GetKeyModAndKeyCode(ptzHotk, uMod, uKey);

	HotkCount++;
	UStrPrint(tzValueName, TEXT("HotKey%d"), HotkCount);
	UStrPrint(tzBinding, TEXT("%s,%d,%d"), ptzCommand, uMod, uKey);
	SHSetValue(HKEY_LOCAL_MACHINE, PELOGON, tzValueName, REG_SZ, tzBinding,
		UStrLen(tzBinding) * sizeof(TCHAR));
	NotifyHotkeysChanged();
	return S_OK;
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
// MD5 of the text, to the clipboard.
//////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT Md5c(CoreContext& ctx, PTSTR ptzCmd)
{	
	PTSTR pMD5Code = UStrToMD5(ptzCmd);
	// MAX_NAME, not the digest length: the caption is a resource string, and
	// LoadString was being told it had room for eighty characters.
	TCHAR ptzCaption[MAX_NAME];
	HRESULT hrClip = SetClipboardText(pMD5Code);
	if (SUCCEEDED(hrClip))
	{
		LoadString(g_hInst, IDS_SaveToClipboard, ptzCaption, _NumOf(ptzCaption));
	}
	else
	{
		LoadString(g_hInst, IDS_MD5String, ptzCaption, _NumOf(ptzCaption));
	}
	MessageBox(NULL,pMD5Code,ptzCaption, MB_OK);
	return hrClip;
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
HRESULT Logo(CoreContext& ctx, PTSTR);
