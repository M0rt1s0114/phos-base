

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Header
#pragma once
#include <Windows.h>
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Program information
#define STR_AppName			TEXT("Phosphor")
#define STR_AppDesc			TEXT("WinPE startup commander")
#define STR_FileName			TEXT("phos.exe")
#define STR_Company			TEXT("Mortis0114 & Kazusa1085")
#define STR_Copyright			TEXT("Copyright (C) 2026 ") STR_Company
#define STR_License			TEXT("Licensed under the GNU General Public License v3.0")
#define STR_Comments			TEXT("Phosphor - an open WinPE startup commander")
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Version information
#define _MakeStr(v)			#v
#define _MakeVer(a, b, c, d)		_MakeStr(a.b.c.d)
#define VER_Major			0
#define VER_Minor			2
#define VER_Release			0
#define VER_Build			1
#define VER_Version			MAKELONG(MAKEWORD(VER_Major, VER_Minor), VER_Release)
#define STR_Version			TEXT(_MakeVer(VER_Major, VER_Minor, VER_Release, VER_Build))
// What the UI shows. A pre-release tag cannot live in four integers, so the
// display string and the version resource stay separate on purpose.
#define STR_VersionText			TEXT("0.2.0-beta2")
#define STR_BuildStamp			TEXT(__DATE__) TEXT(" ") TEXT(__TIME__)
#if defined(_M_X64)
#define STR_Architecture		"amd64"
#define STR_VersionStamp		STR_VersionText TEXT(" X64")
#elif defined(_M_IA64)
#define STR_Architecture		"ia64"
#define STR_VersionStamp		STR_VersionText TEXT(" IA64")
#elif defined(WIN32_PLATFORM_WFSP)
#define STR_VersionStamp		STR_VersionText //TEXT(" SP")
#elif defined(WIN32_PLATFORM_PSPC)
#define STR_VersionStamp		STR_VersionText
#else
#define STR_Architecture		"x86"
#ifdef _UNICODE
#define STR_VersionStamp		STR_VersionText TEXT(" X86U")
#else
#define STR_VersionStamp		STR_VersionText TEXT(" X86")
#endif
#endif
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
