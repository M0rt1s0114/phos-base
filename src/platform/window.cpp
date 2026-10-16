// Paint the splash text. The LOGO window's paint handler calls this, so everything
// it needs is already in the globals TEXT filled in: the string, its rectangle, the
// colour and the font.
HRESULT OutPutText(HDC hDC)
{
	int nDrawn;

	SetBkMode(hDC, TRANSPARENT);
	SetTextColor(hDC, s_crText);
	SelectObject(hDC, hfont ? hfont : GetStockObject(DEFAULT_GUI_FONT));
	nDrawn = DrawText(hDC, s_tzText, -1, &s_rtText, DT_LEFT | DT_WORDBREAK);

	// Without a splash window the text is going somewhere else, and the wait below
	// has to be released rather than wait for a window that will never paint.
	if (!s_hLogo)
	{
		tzDrawTextDone = TRUE;
	}
	return nDrawn ? S_OK : S_FALSE;
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// Wait for the next message, or dwSlice milliseconds, then run what is queued.
//
// This is the "sleep, but keep the window alive" shape the loops below need: a
// bare PeekMessage/DispatchMessage pair with neither a wait nor a sleep spins a
// core for the whole delay, and both the splash and the boot chain sit in these
// loops. QS_ALLINPUT so a paint counts as something to wake up for -- the splash
// text is drawn by this thread, so sleeping through the paint would wait out the
// whole timeout every time.
static VOID PumpMessages(DWORD dwSlice)
{
	MSG msg;

	MsgWaitForMultipleObjectsEx(0, NULL, dwSlice, QS_ALLINPUT, 0);
	while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
	{
		DispatchMessage(&msg);
	}
}

// A millisecond delay that keeps the message queue moving: this runs on the thread
// that owns the splash window, so sleeping without pumping would freeze it.
HRESULT TimeDelay(DWORD WaitTime)
{
	DWORD dwStart = GetTickCount();

	while ((GetTickCount() - dwStart) < WaitTime)
	{
		// 10 ms: short enough that a paint is not visibly late, long enough that
		// this thread is not the thing keeping a core busy.
		PumpMessages(10);
	}
	return S_OK;
}
///////////////////////////////////////////////////////////////////////////////////////////////////


///////////////////////////////////////////////////////////////////////////////////////////////////

// Upstream compared the drawn LOGO text against the MD5 of its own string and
// only then set bRealBoot - and bRealBoot gates the shell registration in SHEL
// and the wallpaper write in WALL. That is the same "is this the author's own
// build" question as CheckMERes, and about our own text it can only answer no,
// which would switch the shell handover off the moment the LOGO text stopped
// being his. For our own text the answer is yes.
void SetFirstTextDone(PTSTR ptzText)
{
	UNREFERENCED_PARAMETER(ptzText);
	Sleep(1000);
	bRealBoot = TRUE;
}
///////////////////////////////////////////////////////////////////////////////////////////////////


///////////////////////////////////////////////////////////////////////////////////////////////////
// Pump messages until the splash text has been painted, or half a second has
// gone by, or bRealBoot says this is a real boot. Pumping is the point: the
// painting happens on this thread's message queue, so a plain Sleep would never
// let it through.
void WaitDrawText(void)
{
	DWORD dwStarted = GetTickCount();

	while ((((GetTickCount() - dwStarted) < 500) && (!tzDrawTextDone)) || (!bRealBoot))
	{
		PumpMessages(10);
	}
}
///////////////////////////////////////////////////////////////////////////////////////////////////


///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// The splash window's own procedure. Three messages matter: the first paint (the
// background and then the text), a paint of a loaded picture, and the close
// request, which fades the window out.
static void LogoInitDialog(HWND hWnd)
{
	s_hLogo = hWnd;
	s_rtScreen.right = GetSystemMetrics(SM_CXSCREEN);
	s_rtScreen.bottom = GetSystemMetrics(SM_CYSCREEN);
	SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, s_rtScreen.right, s_rtScreen.bottom, 0);
	SetLayeredWindowAttributes(hWnd, 0, 255, LWA_ALPHA);
}

static void LogoPaint(HWND hWnd)
{
	PAINTSTRUCT ps;

	BeginPaint(hWnd, &ps);
	if (!ExistLogoImage)
	{
		HBRUSH hBrush = CreateSolidBrush(BackgroundColor);

		// The first paint of a run with no picture covers the screen; every later
		// one covers only the text's rectangle, so whatever is behind it stays.
		tzRECT = tzColorSet ? &s_rtText : &s_rtScreen;
		tzColorSet = true;
		FillRect(ps.hdc, tzRECT, hBrush);
		DeleteObject(hBrush);
	}
	OutPutText(ps.hdc);
	EndPaint(hWnd, &ps);
	tzDrawTextDone = TRUE;
}

// Fade the splash out on the way to the desktop. The sleep belongs at the end of
// each step, so the last alpha is on screen before the loop ends.
static void LogoFadeOut(HWND hWnd)
{
	for (UINT i = 0; i <= 255; i += 5)
	{
		SetLayeredWindowAttributes(hWnd, 0, (BYTE) (255 - i), LWA_ALPHA);
		Sleep(20);
	}
}

INT_PTR CALLBACK LogoProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	UNREFERENCED_PARAMETER(lParam);

	switch (uMsg)
	{
	case WM_DESTROY:
		PostQuitMessage(0);
		break;

	case WM_PAINT:
		LogoPaint(hWnd);
		break;

	case WM_ERASEBKGND:
		// A loaded picture is what erases the background: GDI+ draws it over the
		// whole screen, and the NULL brush says there is nothing left to do.
		if (s_hImage)
		{
			HANDLE hGraph;

			if (pGdipCreateFromHDC((HDC) wParam, &hGraph) == S_OK)
			{
				pGdipDrawImageRectI(hGraph, s_hImage, 0, 0, s_rtScreen.right, s_rtScreen.bottom);
				return (INT_PTR) GetStockObject(NULL_BRUSH);
			}
		}
		break;

	case WM_INITDIALOG:
		LogoInitDialog(hWnd);
		break;

	case WM_COMMAND:
		if (wParam == IDCANCEL)
		{
			LogoFadeOut(hWnd);
			s_hLogo = NULL;
			return EndDialog(hWnd, IDCLOSE);
		}
		break;
	}

	return FALSE;
}
////////////////////////////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
HRESULT Boot(BOOL);
// Wait for the splash window to appear. The counter starts below zero, so the wait
// is RETRY + 180 steps of 50ms; when no window ever turns up, the error box takes
// over and offers a reboot rather than leaving a black screen.
//
// It used to take "redraw the splash first" as an argument, which the one caller
// never asked for: the branch went through the text LOGO command with an empty
// image and was dead.
void WaitLogo(void)
{
	{
		INT i = -180;

		while (!s_hLogo && (i < RETRY))
		{
			Sleep(50);
			i++;
		}
	}

	if (s_hLogo)
	{
		SetProcessWorkingSetSize(CurrentProcess, (SIZE_T) -1, (SIZE_T) -1);
		return;
	}

	{
		TCHAR tzErr[MAX_NAME];
		TCHAR tzErrTip[MAX_NAME];

		LoadString(g_hInst, IDS_LogoErr, tzErr, _NumOf(tzErr));
		LoadString(g_hInst, IDS_LogoErrTip, tzErrTip, _NumOf(tzErrTip));
		Boot(MessageBox(s_hLogo, tzErr, tzErrTip, MB_YESNO | MB_ICONHAND) == IDYES);
	}
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
#include <TLHelp32.h>
// The image name behind a pid. Answers the buffer it was given, or NULL when no
// process has that pid -- an answer ParentProcessIsME relies on, because a parent
// can already be gone by the time it asks.
PTSTR FindProcName(DWORD ProcID, PTSTR ProcessName, UINT cchName)
{
	HANDLE hSnap;
	PROCESSENTRY32 pe;
	PTSTR ptzResult = NULL;

	hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hSnap == INVALID_HANDLE_VALUE)
	{
		return NULL;
	}

	pe.dwSize = sizeof(pe);
	for (BOOL bMore = Process32First(hSnap, &pe); bMore; bMore = Process32Next(hSnap, &pe))
	{
		if (pe.th32ProcessID != ProcID)
		{
			continue;
		}
		// The caller's buffer is the caller's size: an image name that does not
		// fit is answered with nothing rather than with a shortened one.
		if ((UStrLen(pe.szExeFile) + 1) > cchName)
		{
			break;
		}
		UStrCopy(ProcessName, pe.szExeFile);
		ptzResult = ProcessName;
		break;
	}
	CloseHandle(hSnap);
	return ptzResult;
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// Park the TEXT settings and put them back afterwards: DisplayAppDesc draws its
// own line in a corner, and that line's geometry must not be left behind for the
// next TEXT call to inherit.
void SaveUserTextData(BOOL tzSave)
{
	if (tzSave)
	{
		tzOldFontSize = fontsize;
		Old_s_crText = s_crText;
		tzOldTsRECT = s_rtText;
	}
	else
	{
		fontsize = tzOldFontSize;
		s_crText = Old_s_crText;
		s_rtText = tzOldTsRECT;
	}
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
DWORD FindProc(PCTSTR);
BOOL ParentProcessIsME(void)
{
	BOOL bResult = TRUE;
	TCHAR ParentProcessName[MAX_NAME];
	TCHAR tzSelf[MAX_PATH];
	TCHAR tzSelfName[MAX_NAME];
	PCTSTR apszOurs[3];
	UINT uCount = 0;
	PTSTR ptzParent = FindProcName((DWORD) FindProc(NULL), ParentProcessName,
		_NumOf(ParentProcessName));

	// "Ours" is this image's own module name: the product is phos.exe, and a
	// second copy started by a script has to be recognised as ours. Two literal
	// names used to be listed here instead, and they are gone: the release image
	// carries neither, so all they could still match was somebody else's file.
	// Getting this wrong is silent rather than loud: it flips bRealBoot, which
	// is what gates the shell registration in SHEL.
	tzSelfName[0] = 0;
	if (GetModuleFileName(g_hInst, tzSelf, _NumOf(tzSelf)))
	{
		PTSTR pSlash = UStrRChr(tzSelf, '\\');

		// Same rule as the caller's buffer above: a name that does not fit is
		// answered with nothing rather than with a shortened one. The leaf of a
		// MAX_PATH path can be far longer than MAX_NAME, so this is not a copy
		// that can be assumed to fit.
		if (!UStrCopyTo(tzSelfName, pSlash ? (pSlash + 1) : tzSelf))
		{
			tzSelfName[0] = 0;
		}
	}
	apszOurs[uCount++] = tzSelfName;

	if (ptzParent)
	{
		bResult = FALSE;
		for (UINT i = 0; i < uCount; i++)
		{
			if (apszOurs[i][0] && !UStrCmpNI(apszOurs[i], ptzParent, UStrLen(ptzParent)))
			{
				bResult = TRUE;
				break;
			}
		}
	}
	return bResult;
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// The shell's image name. The "is the desktop up yet" test below uses it, and so
// does the tray's watcher.
TCHAR EXPLORER[] = TEXT("EXPLORER.EXE");
// Defined below, next to the text it draws. Declared here because the splash's
// own description is drawn before it.
HRESULT SetLogoText(PCTSTR ptzText, DWORD dwColor, INT iLeft, INT iTop, INT iRight,
	INT iBottom, INT iSize, BOOL bKeepPrevious);

// The build's own line in the corner of the splash, drawn only when this process is
// the one the shell chain started and explorer is not up yet. It brings its own
// font and geometry, which are parked and restored around it.
void DisplayAppDesc(void)
{
	BOOL bShow = (ParentProcessIsME() && !FindProc(EXPLORER));

	bRealBoot = !bShow;
	if (!bShow)
	{
		return;
	}

	SaveUserTextData(TRUE);
	fontsize = 14;
	s_rtText.left = 8;
	s_rtText.top = 8;
	s_rtText.right = 400;
	s_rtText.bottom = 48;
	s_crText = 0xFFFFFF;
	tzDrawTextDone = FALSE;
	// The settings just written are the whole of it: the text capability's
	// defaults, spelled out. It used to go through the text TEXT command, which
	// parsed the description for field letters (L/T/R/B/$/*/#) and would have cut
	// it short at one of them -- the description has none today, and a future one
	// that did would have been silently truncated rather than drawn.
	SetLogoText(LogoDescText, 0, -1, -1, -1, -1, -1, FALSE);
	SaveUserTextData(FALSE);
}
//////////////////////////////////////////////////////////////////////////////////////////////////


////////////////////////////////////////////////////////////////////////////////////////////////////
// The splash-text capability.
//
// A setting the caller does not name is left alone, which is how the legacy
// parser behaves: every TEXT call reuses whatever the previous one left behind.
// -1 means "not named" for the numbers, and a colour of 0 says the same,
// because the reference only accepts a positive one.
HRESULT SetLogoText(PCTSTR ptzText, DWORD dwColor, INT iLeft, INT iTop, INT iRight,
	INT iBottom, INT iSize, BOOL bKeepPrevious)
{
	static UINT uOldFontSize = 0;
	TCHAR tzText[MAX_STR];
	PTSTR ptzBody;
	BOOL bClear;
	HRESULT hResult = S_OK;

	if ((dwColor > 0) && (s_crText != (COLORREF) dwColor))
	{
		s_crText = (COLORREF) dwColor;
	}
	if (iLeft >= 0)   { s_rtText.left = iLeft; }
	if (iTop >= 0)    { s_rtText.top = iTop; }
	if (iRight >= 0)  { s_rtText.right = iRight; }
	if (iBottom >= 0) { s_rtText.bottom = iBottom; }
	if (iSize >= 0)   { fontsize = (UINT) iSize; }

	if (!UStrCopyTo(tzText, (ptzText != NULL) ? ptzText : TEXT("")))
	{
		return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
	}
	ptzBody = UStrTrim(tzText);
	bClear = (*ptzBody == 0);
	ptzBody = USplitStr(ptzBody);

	// Upstream folds "do not use this text" into the pointer by multiplying it
	// by the flag. That needs a pointer to fit in a DWORD, so on x64 it handed
	// UStrCopy a truncated address - an access violation, every time. Write what
	// the arithmetic meant instead: copy the string, or leave the destination
	// empty.
	//
	// s_tzText is eighty characters and lives next to the colour, the font size
	// and the font handle in the boot context: an over-long splash line used to
	// run over the end of it, and the crash that followed was in whatever got
	// overwritten. Refuse the text instead.
	if (!UStrCopyTo(s_tzText, ptzBody))
	{
		return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
	}

	if (fontsize && (uOldFontSize != fontsize))
	{
		if (hfont)
		{
			DeleteObject(hfont);
		}
		hfont = CreateFont(fontsize, 0, 0, 0, 400, 0, 0, 0,
			ANSI_CHARSET, OUT_DEFAULT_PRECIS,
			CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
			DEFAULT_PITCH | FF_DONTCARE, deffontname);
		uOldFontSize = fontsize;
	}

	if (bClear || (!bKeepPrevious))
	{
		hResult = InvalidateRect(s_hLogo, &s_rtText, TRUE) ? S_OK : S_FALSE;
	}
	if ((!s_hLogo) || (!bClear))
	{
		hResult = OutPutText(GetDC(s_hLogo));
	}

	if (!bRealBoot)
	{
		// SetFirstTextDone ignores its argument, so pass NULL rather than hand a
		// thread a pointer into a stack frame that may not outlive the call.
		UThreadClose(UThreadCreate((UPROC) SetFirstTextDone, NULL));
	}

	if (!tzDrawTextDone)
	{
		WaitDrawText();
	}

	return hResult;
}

// TEXT <text>[#<colour>][L<x>][T<y>][R<right>][B<bottom>][$<size>][*]
//
// The field letters are looked for from the right, and the **first one found**
// is where the text ends: everything from there on is the field list, everything
// before it is what gets drawn. Searching from the right is what keeps a letter
// inside the text itself from being read as a field.
//
// The field list is uppercased only when there is a '#', because the uppercasing
// starts at it -- so "TEXT helloL10" works and "TEXT hellol10" does not. That is
// the reference's shape, kept as it is.
//
// '*' is the odd one: it takes no value, and means "do not clear what was drawn
// before", which is how a caller stacks lines.
////////////////////////////////////////////////////////////////////////////////////////////////////


////////////////////////////////////////////////////////////////////////////////////////////////////
// The splash window's caption travels inside a hand-built DLGTEMPLATE, so each
// string is a fixed-size array. The caption carries our name, and the array is
// sized from the literal so the count cannot drift. The zero padding after it is
// unchanged.
const struct {DLGTEMPLATE dtDlg; WCHAR  wzMenu[1]; WCHAR wzClass[1]; WCHAR wzCaption[_NumOf(L"Phosphor")]; WORD wEnd[6];} c_dtLogo =
{{WS_POPUP | WS_VISIBLE | DS_CENTER, WS_EX_LAYERED | WS_EX_TOOLWINDOW, 0, 0, 0, 400, 300}, 0, 0, L"Phosphor"};
// LOGO <image>[,<background>] -- the splash.
//
// The value after the comma is the background colour; when it is 0 the registry
// decides (GetBackgroundColor). A path that does not exist is turned into an
// empty one, which is how the no-picture form gets its window anyway.
//
// With a window already up this only swaps the image. Otherwise it brings GDI+
// in -- from System32 first, and failing that out of WinSxS by hand, because
// some images carry it only there -- loads the picture through it, and shows the
// dialog. Whatever it loaded is released once the dialog returns.
// GDI+ is normally in System32; some images only carry it under WinSxS, which is
// why a failed LoadLibrary sends this through that folder by hand rather than
// giving up on the picture.
static HMODULE LoadGdiPlus(void)
{
	HMODULE hLib = LoadLibrary(TEXT("GdiPlus.dll"));

	if (hLib)
	{
		return hLib;
	}

	{
		WIN32_FIND_DATA fd;
		TCHAR tzPattern[MAX_PATH];
		HANDLE hFind;

		ExpandEnvironmentStrings(TEXT("%SystemRoot%\\WinSxS\\*"), tzPattern, _NumOf(tzPattern));
		hFind = FindFirstFile(tzPattern, &fd);
		if (hFind == INVALID_HANDLE_VALUE)
		{
			return NULL;
		}

		UDirSplitPath(tzPattern);       // the pattern turns into the folder it sat in
		do
		{
			TCHAR tzPath[MAX_PATH];

			if ((fd.cFileName[0] == '.') || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			{
				continue;
			}
			if ((UStrLen(tzPattern) + UStrLen(fd.cFileName) + 15) >= _NumOf(tzPath))
			{
				continue;		// a path that does not fit cannot name the file
			}
			UStrPrint(tzPath, TEXT("%s\\%s\\GdiPlus.dll"), tzPattern, fd.cFileName);
			hLib = LoadLibrary(tzPath);
		}
		while (!hLib && FindNextFile(hFind, &fd));
		FindClose(hFind);
	}
	return hLib;
}

// The entry points this file calls, by name. Nothing links against GDI+'s import
// library, so on an image without it every one of these stays NULL and the calls
// below are skipped.
static const struct
{
	PCSTR	pszName;
	FARPROC	*pfn;
} c_GdiplusEntries[] =
{
	{ "GdiplusStartup",        (FARPROC *) &pGdiplusStartup },
	{ "GdiplusShutdown",       (FARPROC *) &pGdiplusShutdown },
	{ "GdipLoadImageFromFile", (FARPROC *) &pGdipLoadImageFromFile },
	{ "GdipDisposeImage",      (FARPROC *) &pGdipDisposeImage },
	{ "GdipCreateFromHDC",     (FARPROC *) &pGdipCreateFromHDC },
	{ "GdipDrawImageRectI",    (FARPROC *) &pGdipDrawImageRectI },
};

static void BindGdiPlus(HMODULE hLib)
{
	for (UINT i = 0; i < _NumOf(c_GdiplusEntries); i++)
	{
		*c_GdiplusEntries[i].pfn = hLib ? GetProcAddress(hLib, c_GdiplusEntries[i].pszName) : NULL;
	}
}

// LOGO <image>[,<background>] -- the splash.
//
// The value after the comma is the background colour; 0 means "ask the registry"
// (GetBackgroundColor). A picture that is not there is the same as no picture: the
// window still comes up, which is what the "splash only" call asks for.
//
// With a window already on screen this only swaps the picture. Otherwise GDI+ is
// brought in, the image is loaded through it, and whatever was loaded is released
// once the dialog returns.
BOOL WINAPI LogoCMD(PTSTR ptzCmd)
{
	PTSTR ptzArg = UStrChr(ptzCmd, ',');
	HMODULE hLib = NULL;
	HANDLE hToken = NULL;
	WCHAR wzPath[MAX_PATH];

	if (ptzArg)
	{
		*ptzArg++ = 0;
		BackgroundColor = UStrToInt(ptzArg);
	}
	if (!BackgroundColor)
	{
		BackgroundColor = GetBackgroundColor();
	}

	if (!UFileExist(ptzCmd))
	{
		ptzCmd[0] = 0;
	}

	if (ptzCmd[0])
	{
		UStrToWStr(wzPath, ptzCmd, _NumOf(wzPath));
		ptzCmd[0] = 0;
		ExistLogoImage = true;

		if (s_hLogo)
		{
			// Already up: swap the picture and repaint. GDI+ is loaded by then, so
			// there is nothing to bring in or to release here.
			if (s_hImage)
			{
				pGdipDisposeImage(s_hImage);
				s_hImage = NULL;
			}
			if (pGdipLoadImageFromFile)
			{
				pGdipLoadImageFromFile(wzPath, &s_hImage);
			}
			return InvalidateRect(s_hLogo, NULL, TRUE);
		}

		hLib = LoadGdiPlus();
		BindGdiPlus(hLib);
		if (pGdipLoadImageFromFile)
		{
			GPSTART s = {1};

			pGdiplusStartup(&hToken, &s, NULL);
			pGdipLoadImageFromFile(wzPath, &s_hImage);
		}
	}

	DialogBoxIndirect(g_hInst, (LPCDLGTEMPLATE) &c_dtLogo, NULL, (DLGPROC) LogoProc);

	if (hLib)
	{
		if (s_hImage)
		{
			pGdipDisposeImage(s_hImage);
			s_hImage = NULL;
		}
		if (pGdiplusShutdown)
		{
			pGdiplusShutdown(hToken);
		}
		FreeLibrary(hLib);
	}
	return TRUE;
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// Get privilege

//////////////////////////////////////////////////////////////////////////////////////////////////
// Which desktop are we on, and which one is being displayed?
//
// This is the question behind the flat-screen report: the shell processes are
// running, nothing the boot process tree creates is visible, and the only code
// that decides which desktop is on screen is the OpenDesktop / SetThreadDesktop
// / SwitchDesktop triple in Main() - where every call can fail and every
// failure is silent.
//
// OpenInputDesktop answers "what is the user looking at"; GetThreadDesktop
// answers "where is this process". Different names mean the whole chain is
// running somewhere nobody can see it.
//////////////////////////////////////////////////////////////////////////////////////////////////
static VOID UoiName(HANDLE hObject, PTSTR ptzOut, UINT uOut)
{
	DWORD dwNeeded = 0;

	ptzOut[0] = 0;
	if (hObject && GetUserObjectInformation(hObject, UOI_NAME, ptzOut, uOut * sizeof(TCHAR), &dwNeeded))
	{
		return;
	}
	UStrCopyN(ptzOut, TEXT("?"), uOut);
}

VOID DesktopNamesText(PTSTR ptzOut, UINT uOut)
{
	TCHAR tzInput[MAX_NAME];
	TCHAR tzThread[MAX_NAME];
	TCHAR tzWinSta[MAX_NAME];
	TCHAR tzShellCls[MAX_NAME];
	HDESK hInput = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_SWITCHDESKTOP);
	HDESK hThread = GetThreadDesktop(GetCurrentThreadId());
	HWINSTA hWinSta = GetProcessWindowStation();
	HWND hShell = GetShellWindow();
	DWORD dwShellPid = 0;
	DWORD dwExplorerPid = (DWORD) FindProc(TEXT("explorer.exe"));

	UoiName(hInput, tzInput, _NumOf(tzInput));
	UoiName(hThread, tzThread, _NumOf(tzThread));
	UoiName(hWinSta, tzWinSta, _NumOf(tzWinSta));
	if (!hInput)
	{
		UStrCopy(tzInput, TEXT("<none>"));
	}

	// GetShellWindow() is the desktop itself - the window a process owns once
	// Windows accepts it as the shell. If that handle is null, or its pid is not
	// the explorer.exe that is running, then explorer is merely a process and
	// nobody owns the desktop. That is the difference between "explorer is
	// running" (a pid in the list) and "there is a desktop" (something to draw).
	UStrCopy(tzShellCls, TEXT("<none>"));
	if (hShell)
	{
		if (!GetClassName(hShell, tzShellCls, _NumOf(tzShellCls)))
		{
			UStrCopy(tzShellCls, TEXT("?"));
		}
		GetWindowThreadProcessId(hShell, &dwShellPid);
	}

	UStrPrintN(ptzOut, uOut, TEXT("input=%s thread=%s winsta=%s shell=%s/%u explorer=%u"),
		tzInput, tzThread, tzWinSta, tzShellCls, (UINT) dwShellPid, (UINT) dwExplorerPid);
	if (hInput)
	{
		CloseDesktop(hInput);
	}
}
