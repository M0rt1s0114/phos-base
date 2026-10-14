// Answer a MESS box from another thread once its time is up: find the window by
// its caption, check that it is ours (its parent is the splash window), bring it
// forward, wait the configured time and then press the chosen button.
//
// The loop counts to RETRY but its condition is "or no window yet", so it keeps
// looking until it finds one -- the timeout this implements is the caller's, not
// this loop's.
HWND CloseMessageBox(PTSTR ptzCaption)
{
	HWND hBox;

	// Look until a window with that caption exists. The loop has no counter of
	// its own: what ends it is the caller's timeout.
	do
	{
		hBox = FindWindow(NULL, ptzCaption);
	}
	while (!hBox);

	if (hBox && (GetParent(hBox) == s_hLogo))       // one of our own boxes
	{
		SetWindowPos(hBox, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_SHOWWINDOW);
		SetForegroundWindow(hBox);
		SetActiveWindow(hBox);
		Sleep(MessageWaitTime);
		SendMessage(hBox, WM_COMMAND, MessageAnswer, 0);
	}
	return hBox;
}
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
// MESS <text>@<caption>[#<type>][*<timeout>][$<default>]
//
// The splits happen in that order: '@' separates the text from the caption, '#'
// the caption from the type, and inside the type '*' asks for a timeout while
// '$' picks which button gets pressed when it fires. A type of "OK" is a plain
// box; anything else is Yes/No, and the answer goes back through the environment
// as "YESNO=YES" or "YESNO=NO".
//
// With a timeout the box is answered from another thread (CloseMessageBox), which
// is why the answer and the wait live in MessageAnswer and MessageWaitTime rather
// than being passed along.
HRESULT Mess(CoreContext& ctx, PTSTR ptzCmd)
{
	PTSTR ptzCaption = UStrChr(ptzCmd, '@');
	PTSTR ptzType;
	BOOL bYesNo;
	PTSTR ptzWait;
	PTSTR ptzAnswer;

	if (!ptzCaption)
	{
		return S_FALSE;                 // no caption, no box
	}
	*ptzCaption++ = 0;

	ptzType = UStrChr(ptzCaption, '#');
	if (!ptzType)
	{
		return S_FALSE;                 // no type, no box
	}
	*ptzType++ = 0;

	// The timeout, if the type carries one, lives in MessageWaitTime because the
	// thread that answers the box reads it later. Its button lives in
	// MessageAnswer for the same reason.
	MessageWaitTime = 0;
	ptzWait = UStrChr(ptzType, '*');
	if (ptzWait)
	{
		MessageWaitTime = UStrToInt(ptzWait + 1);
	}

	MessageAnswer = IDYES;
	bYesNo = (UStrCmpNI(ptzType, TEXT("OK"), 2) != 0);
	if (bYesNo)
	{
		ptzAnswer = UStrChr(ptzType, '$');
		if (ptzAnswer)
		{
			UStrToUpper(ptzAnswer + 1);
			if (ptzAnswer[1] == 'N')
			{
				MessageAnswer = IDNO;
			}
		}
	}

	ptzCmd = USplitStr(ptzCmd);

	if (MessageWaitTime)
	{
		UThreadClose(UThreadCreate((UPROC) CloseMessageBox, ptzCaption));
	}

	if (bYesNo)
	{
		TCHAR tzAnswer[MAX_NAME];

		UStrCopy(tzAnswer, TEXT("YESNO="));
		UStrCat(tzAnswer, (MessageBox(s_hLogo, ptzCmd, ptzCaption, MB_YESNO | MB_ICONQUESTION) == IDYES)
			? TEXT("YES") : TEXT("NO"));
		SetEnvironment(tzAnswer, FALSE, FALSE);
	}
	else
	{
		MessageBox(s_hLogo, ptzCmd, ptzCaption, MB_ICONINFORMATION);
	}
	return S_OK;
}
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
// The picker starts where the caller asked: BFFM_INITIALIZED is the first moment
// the control exists and can be told.
int CALLBACK BrowseCallbackProc(HWND hwnd, UINT uMsg, LPARAM lParam, LPARAM lpData)
{
	UNREFERENCED_PARAMETER(lParam);
	UNREFERENCED_PARAMETER(lpData);

	if (uMsg == BFFM_INITIALIZED)
	{
		SendMessage(hwnd, BFFM_SETSELECTION, TRUE, (LPARAM) pzInitDirName);
	}
	return S_OK;
}
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
#define TargetRoot TEXT("TARGET_ROOT")
#define TargetDriv TEXT("TARGET_DRIVE")
// Show the shell's folder picker and put the chosen path into InitPath, which the
// caller owns. NULL means nothing was chosen.
//
// The title comes from the caller when it gave one, and from IDS_BrowseTitle when
// it did not. That second case reads into a buffer of this function's own: the
// bi.lpszTitle field has to be a pointer before LoadString is handed it, and it
// is zero at this point in the BROWSEINFO.
PTSTR ShellBrowseFolder(PTSTR InitPath,BOOL IncludeFiles,PTSTR ptzTitle)
{
	TCHAR tzTitle[MAX_NAME];
	BROWSEINFO bi = {0};

	bi.ulFlags = BIF_RETURNONLYFSDIRS| 
		BIF_EDITBOX| 
		BIF_NONEWFOLDERBUTTON| 
		BIF_USENEWUI;
	if (IncludeFiles)
	{
		bi.ulFlags = bi.ulFlags|BIF_BROWSEINCLUDEFILES;
	}

	if (ptzTitle && ptzTitle[0])
	{
		bi.lpszTitle = ptzTitle;
	}
	else
	{
		LoadString(g_hInst, IDS_BrowseTitle, tzTitle, MAX_NAME);
		bi.lpszTitle = tzTitle;
	}

	bi.hwndOwner = s_hLogo;
	bi.lpfn = BrowseCallbackProc;
	ITEMIDLIST *pIDL = SHBrowseForFolder(&bi);
	if (SHGetPathFromIDList(pIDL,InitPath))
	{
		return InitPath;
	}
	else
	{
		return NULL;
	}
}
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
// BROW [*]<initial path>[$<title>]
//
// The leading '*' means "files too" -- it is counted rather than searched for, so
// it has to be first. '$' separates the title from the path. What comes back goes
// into the environment twice: TARGET_ROOT is the whole path and TARGET_DRIVE just
// the drive, which is the path truncated after two characters.
//
// The answer is always S_OK: closing the picker without choosing anything is not
// a mistake, and the environment simply keeps whatever it had.
HRESULT Brow(CoreContext& ctx, PTSTR ptzCmd)
{
	TCHAR tzChosenPath[MAX_STR];
	TCHAR tzPair[MAX_STR];
	BOOL bIncludeFiles = (*ptzCmd != '*');
	PTSTR ptzTitle;
	PTSTR ptzChosen;

	// A leading '*' means "files as well as folders", and it is counted rather than
	// searched for, so the starting path begins one character later.
	pzInitDirName = ptzCmd + (*ptzCmd == '*');

	ptzTitle = UStrChr(ptzCmd, '$');
	if (ptzTitle)
	{
		*ptzTitle++ = 0;
	}

	ptzChosen = ShellBrowseFolder(tzChosenPath, bIncludeFiles, ptzTitle);
	if (ptzChosen)
	{
		// The whole path, in the machine and the process environment both.
		UStrPrint(tzPair, TEXT("%s=%s"), TargetRoot, ptzChosen);
		SetEnvironment(tzPair, TRUE, FALSE);
		SetEnvironment(tzPair, FALSE, FALSE);

		// And the drive on its own: everything past the first two characters goes.
		ptzChosen[2] = 0;
		UStrPrint(tzPair, TEXT("%s=%s"), TargetDriv, ptzChosen);
		SetEnvironment(tzPair, FALSE, FALSE);
	}
	return S_OK;                        // closing the picker is not an error
}
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////
// About box
//
// The box is built from code: no dialog template, no resource, and the text is a
// wide literal in a UTF-8 source, so no code page sits between the source and
// what is on screen.
//
// It is also what a bare phos.exe shows, which makes it the anchor the launch
// smoke test uses: the process stays alive for as long as the box is on screen.
//
// TaskDialog comes from comctl32 version 6, which the manifest selects. Nothing
// links against comctl32, so it is fetched at run time and a system without v6
// falls back to a plain message box rather than failing.
//////////////////////////////////////////////////////////////////////////////////////////////////
#define ABOUT_URL TEXT("https://github.com")
#define IDC_ABOUT_GITHUB 1001
#define IDC_ABOUT_CLOSE  1002

static BOOL g_bAboutDone = FALSE;

static LRESULT CALLBACK AboutProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_CREATE:
		{
			TCHAR tzHeading[MAX_STR];
			TCHAR tzBody[MAX_STR];
			RECT rcClient;
			HWND hChild;
			const int iButtonWidth = 140;
			const int iButtonHeight = 28;
			const int iMargin = 16;

			UStrPrint(tzHeading, TEXT("%s %s"), STR_AppName, STR_VersionStamp);
			UStrPrint(tzBody, TEXT("%s\r\n%s\r\n%s\r\n\r\nBuilt %s"),
				STR_AppDesc, STR_Copyright, STR_License, STR_BuildStamp);

			// Lay the right hand button out against the real client width. The
			// window was sized from a client rectangle, so both margins are the
			// same only if this one is placed relative to the client.
			GetClientRect(hWnd, &rcClient);

			hChild = CreateWindowEx(0, TEXT("STATIC"), tzHeading,
				WS_CHILD | WS_VISIBLE | SS_NOPREFIX, iMargin, 16, rcClient.right - (iMargin * 2), 20, hWnd, NULL, g_hInst, NULL);
			SendMessage(hChild, WM_SETFONT, (WPARAM) GetStockObject(DEFAULT_GUI_FONT), TRUE);
			hChild = CreateWindowEx(0, TEXT("STATIC"), tzBody,
				WS_CHILD | WS_VISIBLE | SS_NOPREFIX, iMargin, 42, rcClient.right - (iMargin * 2), 84, hWnd, NULL, g_hInst, NULL);
			SendMessage(hChild, WM_SETFONT, (WPARAM) GetStockObject(DEFAULT_GUI_FONT), TRUE);

			hChild = CreateWindowEx(0, TEXT("BUTTON"), TEXT("Open GitHub"),
				WS_CHILD | WS_VISIBLE | WS_TABSTOP, iMargin, 136, iButtonWidth, iButtonHeight, hWnd,
				(HMENU) IDC_ABOUT_GITHUB, g_hInst, NULL);
			SendMessage(hChild, WM_SETFONT, (WPARAM) GetStockObject(DEFAULT_GUI_FONT), TRUE);
			hChild = CreateWindowEx(0, TEXT("BUTTON"), TEXT("Close"),
				WS_CHILD | WS_VISIBLE | WS_TABSTOP,
				rcClient.right - iMargin - iButtonWidth, 136, iButtonWidth, iButtonHeight, hWnd,
				(HMENU) IDC_ABOUT_CLOSE, g_hInst, NULL);
			SendMessage(hChild, WM_SETFONT, (WPARAM) GetStockObject(DEFAULT_GUI_FONT), TRUE);
		}
		return 0;

	case WM_COMMAND:
		if (LOWORD(wParam) == IDC_ABOUT_GITHUB)
		{
			if (ShellExecute(NULL, TEXT("open"), ABOUT_URL, NULL, NULL, SW_SHOWNORMAL)
				<= (HINSTANCE) 32)
			{
				// No browser and no association, which is the normal state of a
				// WinPE: show the address so it can be typed or copied by hand.
				MessageBox(hWnd, ABOUT_URL, STR_AppName, MB_OK | MB_ICONINFORMATION);
			}
			return 0;
		}
		if ((LOWORD(wParam) == IDC_ABOUT_CLOSE) || (LOWORD(wParam) == IDCANCEL))
		{
			g_bAboutDone = TRUE;
			return 0;
		}
		break;

	case WM_CLOSE:
		g_bAboutDone = TRUE;
		return 0;
	}
	return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

HRESULT ShowAbout(VOID)
{
	static const TCHAR tzClass[] = TEXT("PhosphorAbout");
	WNDCLASSEX WindowClass = {0};
	RECT rcSize = {0, 0, 352, 214};
	const DWORD dwStyle = WS_CAPTION | WS_SYSMENU | WS_POPUP;
	const DWORD dwExStyle = WS_EX_DLGMODALFRAME;
	int iWidth;
	int iHeight;
	MSG msg;

	WindowClass.cbSize = sizeof(WindowClass);
	WindowClass.lpfnWndProc = AboutProc;
	WindowClass.hInstance = g_hInst;
	WindowClass.hIcon = LoadIcon(g_hInst, (LPCWSTR) IDI_Main);
	WindowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
	WindowClass.hbrBackground = (HBRUSH) (COLOR_BTNFACE + 1);
	WindowClass.lpszClassName = tzClass;
	RegisterClassEx(&WindowClass);

	// Those 352x190 are the client area the layout is written against. The frame
	// around it is extra: passing the client size straight to CreateWindowEx
	// leaves the right hand margin a few pixels narrower than the left.
	AdjustWindowRectEx(&rcSize, dwStyle, FALSE, dwExStyle);
	iWidth = rcSize.right - rcSize.left;
	iHeight = rcSize.bottom - rcSize.top;

	g_bAboutDone = FALSE;
	HWND hWnd = CreateWindowEx(dwExStyle, tzClass, STR_AppName, dwStyle,
		(GetSystemMetrics(SM_CXSCREEN) - iWidth) / 2,
		(GetSystemMetrics(SM_CYSCREEN) - iHeight) / 2,
		iWidth, iHeight, NULL, NULL, g_hInst, NULL);
	if (!hWnd)
	{
		return S_FALSE;
	}

	ShowWindow(hWnd, SW_SHOW);
	UpdateWindow(hWnd);

	// Modal-ish: stay here until a button says otherwise. IsDialogMessage gives
	// Tab, Enter and Esc the behaviour a dialog would have.
	while (!g_bAboutDone && GetMessage(&msg, NULL, 0, 0))
	{
		if (!IsDialogMessage(hWnd, &msg))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	DestroyWindow(hWnd);
	return S_OK;
}

HRESULT Help(CoreContext& ctx, PTSTR ptzCmd)
{
	// HELP takes a background colour and a text colour. There is nothing to
	// colour here, so the arguments are accepted and ignored.
	return ShowAbout();
}
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
