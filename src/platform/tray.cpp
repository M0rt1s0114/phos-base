// The tray: its window, its icon, its menu and the hotkeys that reach them.
//
// Nothing here carries a command line any more. The menu items and the menu's own
// two hotkeys used to be text -- "<mods>#<key>,<command line>" in the string
// table, "<label>,<command line>" in the menu -- which made the tray a second
// entry point for the text dialect, and the reason the dispatcher had to be asked
// whether a string was a command or a program on every key press. A menu item is
// a call below now, and a script's binding is a closure.
//
// The hotkey bindings this file keeps are interpreter values rather than text,
// so the language's types are included here rather than leaned on from the unity
// build's order, which puts the language after this file.
#include "../lang/runtime.h"

// What the menu items call. These live in files the unity build puts after this
// one, which is why the prototypes are repeated here rather than shared through
// a header -- the same way the rest of this build does it.
HRESULT ShowAbout(VOID);
HRESULT ApplyDisplayMode(DWORD uWidth, DWORD uHeight, DWORD uBits, DWORD uHz);
HRESULT EjectDrives(PCTSTR ptzDrive, UINT uRetry, BOOL bCDROM, BOOL bUSB, BOOL bAsk);
HRESULT TempClearFolder(VOID);
HRESULT TempSetFolder(VOID);

//////////////////////////////////////////////////////////////////////////////////////////
// How the hotkey ids are laid out. 0 and 1 belong to the menu's own two (Ctrl+Alt+M
// and the quick 800x600 mode); the script's slots start at 2, and there are eight of
// them. WM_DESTROY clears the whole range, so the count of ids is not the count of
// slots.
#define HOTKEY_SLOTS		8
#define HOTKEY_ID_BASE		2
#define HOTKEY_ID_COUNT		10
#define TRAY_HOTKEY_MENU	0	// Ctrl+Alt+M: open or close the tray menu
#define TRAY_HOTKEY_MODE	1	// Ctrl+Alt+A: the mode the item beside it names
//////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
// The tray menu's own two hotkeys, registered once per process (hence bReg).
//
// These used to come out of the string table as "<mods>#<key>,<command line>", with
// the command half parked in the context and handed to the text dispatcher when the
// key fired. The bindings are here instead, beside the menu items they stand for.
void RegMenuHotk(void)
{
	if (bReg)
	{
		return;
	}

	RegisterHotKey(MainWindowHwnd, TRAY_HOTKEY_MENU, MOD_ALT | MOD_CONTROL, 'M');
	RegisterHotKey(MainWindowHwnd, TRAY_HOTKEY_MODE, MOD_ALT | MOD_CONTROL, 'A');
	bReg = TRUE;
}
//////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
// The hotkeys a script bound, in memory.
//
// These used to be text in the registry: the chain ran in a child process, so
// the only thing that could reach the process owning the window was a string,
// and that string was parsed again on the other side -- an injection surface on
// one hand and a round trip that could go wrong on the other. A binding is a
// closure now, and a closure cannot be written down; the chain runs in this
// process, so it never has to be.
//
// `owner` is what keeps the interpreter -- and with it the scope the closure
// captured -- alive for as long as the binding. Without it the interpreter would
// be gone by the time a key is pressed.
struct HotkeyBinding
{
	UINT                    uVk;
	UINT                    uMods;
	int                     line;		// where it was bound, for the trace
	Lang::FuncPtr           fn;
	std::shared_ptr<Lang::Interp> owner;
};

std::vector<HotkeyBinding> s_hotkeys;

// Defined in host.cpp, which the unity build puts after this file: running the
// closure means calling into the interpreter, and CallFunc lives there.
namespace Lang
{
	void RunHotkeyCallback(size_t slot);
}

// Register the hotkeys the script bound. The id is the slot plus two, so slots 0
// and 1 stay with the menu's own two hotkeys.
void RegHotKey(void)
{
	for (size_t i = 0; i < s_hotkeys.size(); i++)
	{
		TCHAR tzValueName[MAX_NAME];
		BOOL bRegistered = RegisterHotKey(MainWindowHwnd, (int) (i + HOTKEY_ID_BASE),
			s_hotkeys[i].uMods, s_hotkeys[i].uVk);

		// What happened goes back into the registry under a second name, so a boot
		// that cannot register a key says so somewhere a maintainer can read it.
		// The bindings themselves are not there -- they are closures -- so this is
		// what is left of the key as a place to look.
		{
			TCHAR tzResult[MAX_STR];

			UStrPrint(tzValueName, TEXT("RegHotKey%d"), i + 1);
			UStrPrint(tzResult, TEXT("Result=%d; KeyMod=%d, HotKey=%d; closure"),
				bRegistered, s_hotkeys[i].uMods, s_hotkeys[i].uVk);
			SHSetValue(HKEY_LOCAL_MACHINE, PHOS_STATE, tzValueName, REG_SZ, tzResult,
				(UStrLen(tzResult) + 1) * sizeof(TCHAR));
		}
	}

	SHSetValue(HKEY_LOCAL_MACHINE, PHOS_STATE, TEXT("MainWindowHwnd"), REG_DWORD,
		&MainWindowHwnd, sizeof(MainWindowHwnd));
}
//////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
// The menu's items are labels and nothing else now: Resource.rc carries the text
// the user reads, and WM_COMMAND carries the id that says what to do. Splitting
// "<label>,<command line>" on the comma and re-expanding the tail is gone with the
// dialect it fed.
//////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
// Build the tray menu, or drop it.
//
// TRUE: load the resource once and register the hotkey that opens the menu.
// FALSE: tear the submenu down and then the menu, and only clear a handle once its
// DestroyMenu has actually succeeded.
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
			hSubMenu = GetSubMenu(hMenu, 0);
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
// WM_HOTKEY. 0 opens or closes the tray menu, 1 is the quick display mode, and the
// rest are the script's slots -- the id is the slot plus the base they were
// registered under.
static void OnHotKey(WPARAM wParam)
{
	switch (LOWORD(wParam))
	{
	case TRAY_HOTKEY_MENU:
		InsTrayIcon(!hSubMenu);
		break;
	case TRAY_HOTKEY_MODE:
		ApplyDisplayMode(800, 600, 16, 75);
		break;
	default:
		// A slot a script bound: a closure, run in the interpreter that made it.
		Lang::RunHotkeyCallback((size_t) (LOWORD(wParam) - HOTKEY_ID_BASE));
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
// What a menu item does.
//
// The ids are the interface between this and app.rc: the label lives in the
// resource, the action here, and the number is what joins them. The display items
// are a table because there are twelve of them and they differ only in four
// figures.
static const struct
{
	UINT	uId;
	DWORD	uWidth;
	DWORD	uHeight;
	DWORD	uBits;
	DWORD	uHz;
} c_MenuDisplayModes[] =
{
	{ ID_1024_768_32_85,   1024,  768, 32, 85 },
	{ ID_1024_768_32_90,   1024,  768, 32, 90 },
	{ ID_1024_768_16_85,   1024,  768, 16, 85 },
	{ ID_1024_768_16_90,   1024,  768, 16, 90 },
	{ ID_1280_1024_32_85,  1280, 1024, 32, 85 },
	{ ID_1280_1024_32_90,  1280, 1024, 32, 90 },
	{ ID_1280_1024_16_85,  1280, 1024, 16, 85 },
	{ ID_1280_1024_16_90,  1280, 1024, 16, 90 },
	{ ID_800_600_32_85,     800,  600, 32, 85 },
	{ ID_800_600_32_75,     800,  600, 32, 75 },
	{ ID_800_600_16_85,     800,  600, 16, 85 },
	{ ID_800_600_16_75,     800,  600, 16, 75 },
};

// A menu item is a person at the machine, so the two that change something the
// user cannot see -- eject, and the temporary folder -- ask first, and the ones
// that only change a mode do not. Nothing here reports failure: the item has been
// clicked and answered, and there is nowhere left to put an error code.
static void OnMenuCommand(UINT uId)
{
	switch (uId)
	{
	case ID_TRAY_HELP:
		ShowAbout();
		return;

	case ID_EJECT_USB:
		// A person clicked this, so the retry question is one they can answer.
		EjectDrives(NULL, 0, FALSE, TRUE, TRUE);
		return;

	case ID_EJECT_CDROM:
		EjectDrives(NULL, 0, TRUE, FALSE, TRUE);
		return;

	case ID_DEL_TEMP:
		TempClearFolder();
		return;

	case ID_SET_TEMP:
		TempSetFolder();
		return;

	case ID_HIDE_ICON:
		InsTrayIcon(FALSE);
		return;
	}

	for (UINT i = 0; i < _NumOf(c_MenuDisplayModes); i++)
	{
		if (c_MenuDisplayModes[i].uId == uId)
		{
			ApplyDisplayMode(c_MenuDisplayModes[i].uWidth, c_MenuDisplayModes[i].uHeight,
				c_MenuDisplayModes[i].uBits, c_MenuDisplayModes[i].uHz);
			return;
		}
	}
}
//////////////////////////////////////////////////////////////////////////////////////////

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
		// The English tip, once, on the thread that owns the string. The Chinese
		// one is appended in CreateMainWindow, before this thread exists.
		if (NotChineseLangID && !MenuTipText[0])
		{
			TCHAR tzTip[MAX_NAME];

			LoadString(g_hInst, IDS_TrayTip, tzTip, _NumOf(tzTip));
			// Bounded: both halves fit on their own, the sum is not guaranteed
			// to. A tooltip that is one character short is a tooltip; a buffer
			// that runs over is a crash on the boot path.
			UStrCatTo(MenuTipText, tzTip);
			UStrTrim(MenuTipText);
		}
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
		OnMenuCommand((UINT) LOWORD(wParam));
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

		UStrCopyTo(tzChineseTip, AppTrayText);
		// UStrCat is wcscat: a NULL source is a crash, not a no-op, so the flag is
		// tested here. Upstream expressed the condition by multiplying the pointer
		// by the flag, which only works while a pointer fits in a DWORD.
		if (!NotChineseLangID)
		{
			// Bounded for the same reason as the English tip above.
			UStrCatTo(MenuTipText, tzChineseTip);
		}
	}

	return hWindow;
}
//////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
// Progress marker for the shell-locking thread. It has no log channel - it runs
// before any script opens one - and "did it run, what did it see, did it decide
// the shell was already there" is the whole reason a shell that dies after the
// fact never comes back. The registry is where it is left.
static VOID ThreadMark(PCTSTR ptzText)
{
	SHSetValue(HKEY_LOCAL_MACHINE, PHOS_STATE, TEXT("MainThread"), REG_SZ,
		ptzText, (UStrLen(ptzText) + 1) * sizeof(TCHAR));
}

// Watch for the shell this PE is supposed to end up with, and start it again if it
// goes away. Runs as a thread of its own, started from Main.
HRESULT MainThread(void)
{
	TCHAR tzMark[MAX_STR];
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
	UStrPrint(tzFound, TEXT("%u"), (UINT) FindProc(ptzShellName));
	UStrPrint(tzMark, TEXT("shell=%s findproc=%s window=%u check=%u"),
		MyShell, tzFound, MainWindowHwnd ? 1 : 0, 1);
	ThreadMark(tzMark);

	if (MainWindowHwnd && !FindProc(ptzShellName))
	{
		// The tip text is finished by the window thread, in OnFindShell below.
		// This function runs on the shell thread, beside that one, and appending
		// to the string the other is about to hand to Shell_NotifyIcon is two
		// threads on one buffer.
		SendMessage(MainWindowHwnd, FindShellMsg, 0, 0);
	}

	// The shell is expected to stay up, so this watches it for the rest of the
	// process's life and starts it again whenever it is not there.
	//
	// The shape matters and this one was wrong: the loop used to read
	// `while (MyShell[0] && !FindProc(...))`, which runs *while the shell is
	// absent* and therefore stops the moment the shell is seen once. It could
	// never see the shell go away, so "start it again if it exits" was never
	// implemented -- and a shell that came up and then died left the machine with
	// no desktop and no explanation. The tray's own "restart explorer" hotkey
	// depends on the opposite: it kills the shell and expects this loop to put it
	// back.
	while (MyShell[0])
	{
		if (!FindProc(ptzShellName))
		{
			StartShell(MyShell);
		}
		Sleep(200);
	}
	ThreadMark(TEXT("done: the shell registration was withdrawn"));
	return S_OK;
}
//////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
// A hotkey binding has to take effect when it is written, not whenever the shell
// detection thread happens to look.
//
// RegHotKey() is called from exactly one place: the FindShellMsg 0 handler, which
// MainThread sends once, the first time it sees Phosphor\Shell appear. The boot
// chain starts the shell well before Post\Link.phos binds anything, so that read
// found an empty registry and every key stayed dead - F1/F6/F7/F8 did nothing.
//
// A binding made by a script that is not running in the window's process -- a
// Post script is a child of the chain -- asks whichever process owns the tray
// window to re-read the registry. WM_USER carries no pointers, so it is safe to
// pass between processes.
//
// It is *posted*, not sent. A send would wait for the window's thread to process
// it, and while the chain is running that thread is the one blocked here, in the
// spawn that is waiting for this very script: the send would wait for a thread
// that is waiting for it, and the boot would stop for the length of the timeout.
// Posting hands it over and lets the writer carry on; the reader picks it up the
// next time it pumps.
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
		PostMessage(hTray, FindShellMsg, 0, 0);
	}
}

// Called by ui.hotkey. Answers false when every slot is taken: a binding that
// silently vanished would be a key that does nothing and a script that has no
// way to find out.
bool AddHotkeyBinding(UINT uVk, UINT uMods, int line, const Lang::FuncPtr& fn,
	const std::shared_ptr<Lang::Interp>& owner)
{
	HotkeyBinding b;

	if (s_hotkeys.size() >= HOTKEY_SLOTS)
	{
		return false;
	}
	b.uVk = uVk;
	b.uMods = uMods;
	b.line = line;
	b.fn = fn;
	b.owner = owner;
	s_hotkeys.push_back(b);
	NotifyHotkeysChanged();
	return true;
}
//////////////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////////////
// HOTK <mods>#<key>,<command> -- record one binding.
//
// The binding goes into the Phosphor key as "HotKey<n>=<command>,<mods>,<key>", which is
// exactly the shape RegHotKey reads back, and eight is the ceiling for the same
// reason there are eight slots. NotifyHotkeysChanged is what makes it take effect
// now rather than whenever the shell-detection thread happens to look.
//
// The answer is 0 on every path, including the ones that bind nothing: a slot that
// is full and a line with no comma answer the same as a binding that went in. That
// is the reference's shape, and the startup chain binds hotkeys on every boot, so
// the trace has to keep saying 0.
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
// MD5 of the text, to the clipboard. This one was left for later, so the body is
// as it is.
//////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////
