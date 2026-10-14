

//////////////////////////////////////////////////////////////////////////////////////////////////
// Header
#pragma comment(lib, "ShLwAPI.lib")
#pragma comment(lib, "Version.lib")
// SetupAPI is what the ramdisk capability talks to the device tree with; it used
// to be pulled in by the device-install file, which is gone (E10).
#pragma comment(lib, "SetupAPI.lib")
//#pragma comment(lib, "MSVCRT.LIB")  // disabled: pulls in dynamic UCRT

#define _WIN32_WINNT		0x501
#define RETRY				20
#define FindShellMsg		WM_USER + 01
#define CM_nTaskIcoMsg		WM_USER + 02
#define PELOGON				TEXT("SOFTWARE\\PELOGON")
#define LEN_Command			4

#include "identity.h"
#include "core/base.h"
#include "resource.h"
#include "core/strutil.h"
#include "core/disk.h"
#include "core/memory.h"
#include "core/digest.h"
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
// The aliases below keep every name the call sites use working, so no
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
	UINT			HotkCount = 0;
	INT				g_iShowCmd = SW_NORMAL;
	BOOL			TrayIcon = FALSE;
	TCHAR			MyShell[MAX_PATH];
	COLORREF		EditBackColor = 0xFFFFE0;
	COLORREF		EditTextColor = 0x000AC0;
	TCHAR			HotKeyCommand[8][MAX_PATH] = {0};
	HWND			MainWindowHwnd = 0;
	HANDLE			g_hXLog = NULL;
	HANDLE			CurrentProcess = NULL;
	TCHAR			LogFileName[MAX_PATH];
	TCHAR			*LogFileData;
	UINT			LogFileCap = 0;
	PTSTR			pFileStart = NULL;
	UINT			uLineNo = 0;			// source line of the command being run, for the trace
	TCHAR			CurrentDir[MAX_PATH];
	NOTIFYICONDATA	FpNotify;
	TCHAR			MenuCommand[128][MAX_PATH] = {0};
	TCHAR			HotkCmd[MAX_PATH];
	DWORD			MenuFirstID = 0;
	TCHAR			LogoDescText[MAX_NAME] = {0};
	BOOL			NotChineseLangID;
	TCHAR			MenuTipText[MAX_NAME];
	BOOL			g_bInit = FALSE;
	DWORD			LastError = 0;
	LONG_PTR EditDefWndProc;
	HANDLE s_hImage = NULL;
	RECT s_rtText = {4, 4, 640, 64};
	RECT s_rtScreen = {0,0,0,0};
	RECT *tzRECT = NULL;
	UINT UType = 0;
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
	UINT ToalMenu = 0;
		GUID Ramdrive = { 0xFFA1C341, 0x4539, 0x11D3, { 0xB8, 0x8D, 0x00, 0xc0, 0x4f, 0xad, 0x51, 0x72 } };
	UINT MessageWaitTime;
	UINT MessageAnswer;
	PTSTR pzInitDirName;
	GUID USBdrive = {0xA5DCBF10,0x6530,0x11D2,{0x90,0x1F,0x00,0xC0,0x4F,0xB9,0x51,0xED}};
	GUID CDdrive = {0x53F56308,0xb6BF,0x11D0,{0x94,0xF2,0x00,0xA0,0xC9,0x1E,0xFB,0x8B}};
	LOGBRUSH	logbrush;
	HBRUSH	hbrush;
	BOOL bNoSel = FALSE;
	HWND hEdit;
	DWORD timestart_condition = 0;
	UINT s_uExtract = 0;
	BOOL bReg = FALSE;
	HMENU hMenu;
	DWORD timestart_tray = 0;
	DWORD timestart_window = 0;
	RECT tzOldTsRECT;
	UINT tzOldFontSize;
	COLORREF Old_s_crText;
	BOOL g_bXLogStream = FALSE;
	TCHAR RawCommand[MAX_STR];	// the line before expansion, for the few commands
								// that expand their own tail once they have a
								// value to give it
};

CoreContext g_ctx;
auto& logbrush = g_ctx.logbrush;
auto& g_bXLogStream = g_ctx.g_bXLogStream;
auto& hbrush = g_ctx.hbrush;
auto& bNoSel = g_ctx.bNoSel;
auto& hEdit = g_ctx.hEdit;
auto& timestart_condition = g_ctx.timestart_condition;
auto& s_uExtract = g_ctx.s_uExtract;
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
auto& HotkCount = g_ctx.HotkCount;
auto& g_iShowCmd = g_ctx.g_iShowCmd;
auto& TrayIcon = g_ctx.TrayIcon;
auto& MyShell = g_ctx.MyShell;
auto& EditBackColor = g_ctx.EditBackColor;
auto& EditTextColor = g_ctx.EditTextColor;
auto& HotKeyCommand = g_ctx.HotKeyCommand;
auto& MainWindowHwnd = g_ctx.MainWindowHwnd;
auto& g_hXLog = g_ctx.g_hXLog;
auto& CurrentProcess = g_ctx.CurrentProcess;
auto& LogFileName = g_ctx.LogFileName;
auto& LogFileData = g_ctx.LogFileData;
auto& LogFileCap = g_ctx.LogFileCap;
auto& pFileStart = g_ctx.pFileStart;
auto& CurrentDir = g_ctx.CurrentDir;
auto& FpNotify = g_ctx.FpNotify;
auto& MenuCommand = g_ctx.MenuCommand;
auto& HotkCmd = g_ctx.HotkCmd;
auto& MenuFirstID = g_ctx.MenuFirstID;
auto& LogoDescText = g_ctx.LogoDescText;
auto& NotChineseLangID = g_ctx.NotChineseLangID;
auto& MenuTipText = g_ctx.MenuTipText;
auto& g_bInit = g_ctx.g_bInit;
auto& LastError = g_ctx.LastError;
auto& EditDefWndProc = g_ctx.EditDefWndProc;
auto& s_hImage = g_ctx.s_hImage;
auto& s_rtText = g_ctx.s_rtText;
auto& s_rtScreen = g_ctx.s_rtScreen;
auto& tzRECT = g_ctx.tzRECT;
auto& UType = g_ctx.UType;
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
auto& ToalMenu = g_ctx.ToalMenu;
auto& Ramdrive = g_ctx.Ramdrive;
auto& MessageWaitTime = g_ctx.MessageWaitTime;
auto& MessageAnswer = g_ctx.MessageAnswer;
auto& pzInitDirName = g_ctx.pzInitDirName;
auto& USBdrive = g_ctx.USBdrive;
auto& CDdrive = g_ctx.CDdrive;




// These three arrays are the program's own strings, stored as one WORD per
// character with an index-dependent offset (UINT16ToUStr adds i + 1 back). They
// carry this program's own name and version.
const WORD UDescStr[] = // the splash line; the trailing '#' is TEXT's field marker
{
	0x004F,0x0066,0x006C,0x006F,0x006B,0x0062,0x0068,0x006A,
	0x0017,0x0023,0x0015,0x004B,0x005C,0x0060,0x0041,0x0035,
	0x000F,0x0041,0x0061,0x004D,0x005D,0x005E,0x005E,0x0058,
	0x0007,0x0029,0x0054,0x0051,0x0050,0x0043,0x004F,0x0044,
	0x0044,0x0050,0x0009,0xFFFC,0x000B,0x0008,0x000A,0x0006,
	0x0007,0x0003,0x0037,0x0039,0x0047,0x0033,0x0002,0xFFF3
};

const WORD AppTrayText[] = // the tray tooltip
{
	0x004F,0x0066,0x006C,0x006F,0x006B,0x0062,0x0068,0x006A,
	0x0017,0x004D,0x005E,0x0062,0x0043,0x0037,0x0011,0x541F,
	0x5297,0x62F5,0x6312
};



const WORD HelpLabel[] = // the Start Menu shortcut's label
{
	0x0024,0x0051,0x0071,0x005D,0x006D,0x006E,0x0046,0x005D,
	0x0065,0x006B,0x001A,0x0050,0x0043,0x005A,0x0060,0x0063,
	0x005F,0x0056,0x005C,0x005E,0x4F6A,0x7512,0x8BDD,0x65F6,
	0x0013
};




//////////////////////////////////////////////////////////////////////////////////////////////////
// The three WORD arrays above store one character per entry with an
// index-dependent offset: UINT16ToUStr puts back what they took out,
//
//     buf[i] = array[i] + i + 1
//
// so to re-encode a string into one of them, use the inverse:
//
//     array[i] = ch - i - 1
//////////////////////////////////////////////////////////////////////////////////////////////////
