

//////////////////////////////////////////////////////////////////////////////////////////////////
// Header
#pragma comment(lib, "ShLwAPI.lib")
#pragma comment(lib, "Version.lib")
// SetupAPI is what the ramdisk capability talks to the device tree with; it used
// to be pulled in by the device-install file, which is gone (E10).
#pragma comment(lib, "SetupAPI.lib")
//#pragma comment(lib, "MSVCRT.LIB")  // disabled: pulls in dynamic UCRT

// Windows 7 is the floor. The old value said XP, which is not what this runs on
// -- it is a WinPE built from a current Windows -- and it kept the declarations
// for anything newer out of reach, so those calls had to be made through
// GetProcAddress even where they could have been linked. 0x501 also hides
// LOAD_LIBRARY_SEARCH_* and the other post-XP option flags.
#define _WIN32_WINNT		0x601
#define RETRY				20
// Parenthesised: WM_USER + 01 is used as a whole, and an unparenthesised macro
// turns "FindShellMsg * 2" into "WM_USER + 1 * 2".
#define FindShellMsg		(WM_USER + 01)
#define CM_nTaskIcoMsg		WM_USER + 02
#define PHOS_STATE				TEXT("SOFTWARE\\Phosphor")
#define LEN_Command			4

#include "identity.h"
#include "core/base.h"
#include "resource.h"
#include "core/strutil.h"
#include "core/disk.h"
#include "core/memory.h"
// TASKDIALOGCONFIG for the about box. Including the header pulls in no import;
// the dialog itself is fetched from comctl32 at run time.
#include <commctrl.h>
// The capability layer builds its own buffers (SHFileOperation wants a
// double-NUL terminated list), so <vector> has to be visible before the area
// files are pulled into the unit.
#include <vector>

//////////////////////////////////////////////////////////////////////////////////////////////////
// The structured script front end lives in src/lang/. LoadFile hands a file
// that starts with #!phos to it; declared here because script.cpp is
// compiled before the language files are pulled into the unity unit.
//////////////////////////////////////////////////////////////////////////////////////////////////
namespace Lang
{
	BOOL IsMarker(const BYTE *pb, UINT uLen);
	HRESULT RunBytes(const BYTE *pb, UINT uLen, PCTSTR ptzPath);
}

//////////////////////////////////////////////////////////////////
// File-scope mutable state, gathered in one place.
// The references below keep every original name working, so no
// call site had to change. Phase 2 later turns this into a
// context passed explicitly instead of a global instance.
//////////////////////////////////////////////////////////////////
#include <SetupAPI.h>

struct GPSTART {UINT uVersion; PROC pCallback; BOOL bSuppressThread; BOOL bSuppressExternal;};
typedef HRESULT (WINAPI* PGdiplusStartup)(HANDLE* phToken, const GPSTART* gpIn, PVOID pvOut);
typedef HRESULT (WINAPI* PGdiplusShutdown)(HANDLE hToken);
typedef HRESULT (WINAPI* PGdipLoadImageFromFile)(const PWSTR pwzPath, HANDLE* phImage);
typedef HRESULT (WINAPI* PGdipDisposeImage)(HANDLE hImage);
typedef HRESULT (WINAPI* PGdipCreateFromHDC)(HDC hDC, HANDLE* phGraph);
typedef HRESULT (WINAPI* PGdipDrawImageRectI)(HANDLE hGraph, HANDLE hImage, INT iLeft, INT iTop, INT iWidth, INT iHeight);
#define deffontname TEXT("Tahoma")

struct CoreContext
{
			HHOOK			g_hHook = NULL;
	HMENU			hSubMenu;
	HWND			HelphWND;
	HWND			s_hLogo = NULL;
	HINSTANCE		g_hInst = NULL;
	INT				g_iShowCmd = SW_NORMAL;
	BOOL			TrayIcon = FALSE;
	TCHAR			MyShell[MAX_PATH];
	COLORREF		EditBackColor = 0xFFFFE0;
	COLORREF		EditTextColor = 0x000AC0;
	HWND			MainWindowHwnd = 0;
	HANDLE			g_hXLog = NULL;
	HANDLE			CurrentProcess = NULL;
	TCHAR			LogFileName[MAX_PATH];
	TCHAR			*LogFileData;
	UINT			LogFileCap = 0;
	PTSTR			pFileStart = NULL;
	UINT			uLineNo = 0;			// source line of the command being run, for the trace
	NOTIFYICONDATA	FpNotify;
	TCHAR			LogoDescText[MAX_NAME] = {0};
	BOOL			NotChineseLangID;
	// Zeroed: the tip is appended to, and the first append has to see an empty
	// string rather than whatever the loader left in this block.
	TCHAR			MenuTipText[MAX_NAME] = {0};
	BOOL			g_bInit = FALSE;
	DWORD			LastError = 0;
	HANDLE s_hImage = NULL;
	RECT s_rtText = {4, 4, 640, 64};
	RECT s_rtScreen = {0,0,0,0};
	RECT *tzRECT = NULL;
	TCHAR s_tzText[MAX_NAME] = {0};
	COLORREF s_crText = 0xFFFFFF;
	COLORREF BackgroundColor = 0;
	UINT fontsize = 14;
	HFONT hfont = NULL;
	bool tzColorSet = false;
	bool ExistLogoImage = false;
	PGdiplusStartup pGdiplusStartup = NULL;
	PGdiplusShutdown pGdiplusShutdown = NULL;
	PGdipDisposeImage pGdipDisposeImage = NULL;
	PGdipCreateFromHDC pGdipCreateFromHDC = NULL;
	PGdipDrawImageRectI pGdipDrawImageRectI = NULL;
	PGdipLoadImageFromFile pGdipLoadImageFromFile = NULL;
	BOOL tzDrawTextDone = TRUE;
	BOOL bRealBoot = TRUE;
	PTSTR g_ptzDevInf = NULL;
		GUID Ramdrive = { 0xFFA1C341, 0x4539, 0x11D3, { 0xB8, 0x8D, 0x00, 0xc0, 0x4f, 0xad, 0x51, 0x72 } };
	PTSTR pzInitDirName;
	GUID USBdrive = {0xA5DCBF10,0x6530,0x11D2,{0x90,0x1F,0x00,0xC0,0x4F,0xB9,0x51,0xED}};
	GUID CDdrive = {0x53F56308,0xb6BF,0x11D0,{0x94,0xF2,0x00,0xA0,0xC9,0x1E,0xFB,0x8B}};
	LOGBRUSH	logbrush;
	HBRUSH	hbrush;
	HWND hEdit;
	DWORD timestart_condition = 0;
	BOOL bReg = FALSE;
	HMENU hMenu;
	DWORD timestart_tray = 0;
	DWORD timestart_window = 0;
	RECT tzOldTsRECT;
	UINT tzOldFontSize;
	COLORREF Old_s_crText;
	BOOL g_bXLogStream = FALSE;
								// that expand their own tail once they have a
								// value to give it
};

CoreContext g_ctx;
// `--dry-run`, as a process-wide switch rather than context state: it is set
// once from the command line, before anything runs, and only read afterwards.
// Everything the script would change is refused and traced instead.
BOOL g_bDryRun = FALSE;
// One object holds the program's state, and these are references into it. The
// names are kept because the code that reads them was written when they were
// file-scope globals, and the point of the change was to have exactly one place
// that says where the state lives. There is one context per process: this is a
// boot entry rather than a library, and a second instance would be a second
// process.
auto& logbrush = g_ctx.logbrush;
auto& g_bXLogStream = g_ctx.g_bXLogStream;
auto& hbrush = g_ctx.hbrush;
auto& hEdit = g_ctx.hEdit;
auto& timestart_condition = g_ctx.timestart_condition;
auto& bReg = g_ctx.bReg;
auto& hMenu = g_ctx.hMenu;
auto& timestart_tray = g_ctx.timestart_tray;
auto& timestart_window = g_ctx.timestart_window;
auto& tzOldTsRECT = g_ctx.tzOldTsRECT;
auto& tzOldFontSize = g_ctx.tzOldFontSize;
auto& Old_s_crText = g_ctx.Old_s_crText;

auto& g_hHook = g_ctx.g_hHook;
auto& hSubMenu = g_ctx.hSubMenu;
auto& HelphWND = g_ctx.HelphWND;
auto& s_hLogo = g_ctx.s_hLogo;
auto& g_hInst = g_ctx.g_hInst;
auto& g_iShowCmd = g_ctx.g_iShowCmd;
auto& TrayIcon = g_ctx.TrayIcon;
auto& MyShell = g_ctx.MyShell;
auto& EditBackColor = g_ctx.EditBackColor;
auto& EditTextColor = g_ctx.EditTextColor;
auto& MainWindowHwnd = g_ctx.MainWindowHwnd;
auto& g_hXLog = g_ctx.g_hXLog;
auto& CurrentProcess = g_ctx.CurrentProcess;
auto& LogFileName = g_ctx.LogFileName;
auto& LogFileData = g_ctx.LogFileData;
auto& LogFileCap = g_ctx.LogFileCap;
auto& pFileStart = g_ctx.pFileStart;
auto& FpNotify = g_ctx.FpNotify;
auto& LogoDescText = g_ctx.LogoDescText;
auto& NotChineseLangID = g_ctx.NotChineseLangID;
auto& MenuTipText = g_ctx.MenuTipText;
auto& g_bInit = g_ctx.g_bInit;
auto& LastError = g_ctx.LastError;
auto& s_hImage = g_ctx.s_hImage;
auto& s_rtText = g_ctx.s_rtText;
auto& s_rtScreen = g_ctx.s_rtScreen;
auto& tzRECT = g_ctx.tzRECT;
auto& s_tzText = g_ctx.s_tzText;
auto& s_crText = g_ctx.s_crText;
auto& BackgroundColor = g_ctx.BackgroundColor;
auto& fontsize = g_ctx.fontsize;
auto& hfont = g_ctx.hfont;
auto& tzColorSet = g_ctx.tzColorSet;
auto& ExistLogoImage = g_ctx.ExistLogoImage;
auto& pGdiplusStartup = g_ctx.pGdiplusStartup;
auto& pGdiplusShutdown = g_ctx.pGdiplusShutdown;
auto& pGdipDisposeImage = g_ctx.pGdipDisposeImage;
auto& pGdipCreateFromHDC = g_ctx.pGdipCreateFromHDC;
auto& pGdipDrawImageRectI = g_ctx.pGdipDrawImageRectI;
auto& pGdipLoadImageFromFile = g_ctx.pGdipLoadImageFromFile;
auto& tzDrawTextDone = g_ctx.tzDrawTextDone;
auto& bRealBoot = g_ctx.bRealBoot;
auto& g_ptzDevInf = g_ctx.g_ptzDevInf;
auto& Ramdrive = g_ctx.Ramdrive;
auto& pzInitDirName = g_ctx.pzInitDirName;
auto& USBdrive = g_ctx.USBdrive;
auto& CDdrive = g_ctx.CDdrive;




// The program's own strings.
//
// They used to be stored one WORD per character with an index-dependent offset,
// a substitution that hid nothing and took a decoder to read. Re-encoding the
// name by hand had already left a typo in it and pinned the version, so they are
// plain literals now; the splash line takes the version from identity.h instead
// of a copy that goes stale.
//
// The characters that cannot be spelled in ASCII are written as escapes so the
// bytes do not depend on what code page this file happens to be saved in. The
// label below carries no separator: it used to end in the comma the text form of
// LINK wanted between a shortcut and its target, and the capability takes the
// two pieces as arguments.
PCTSTR UDescStr    = TEXT("Phosphor - WinPE Startup Commander, ") STR_VersionText;

PCTSTR AppTrayText = TEXT("Phosphor - WinPE \u542F\u52A8\u6307\u6325");



PCTSTR HelpLabel = TEXT("%StartMenu%\\Phosphor\u4F7F\u7528\u8BF4\u660E");




