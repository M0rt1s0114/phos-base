// The base every file in this tree starts from: the types, the limits and the
// thin Win32 wrappers the rest of the code is written against.
//
// Only what this tree uses is here, and each body is written against the Win32
// call it stands for. The names are the ones the call sites already use.
#pragma once

#include <TChar.h>
#include <StdIO.h>
#include <StdArg.h>
#include <StdLib.h>
#include <String.h>
#include <Memory.h>
#include <Windows.h>
#include <ShLwApi.h>
// The safe-string layer. STRSAFE_NO_DEPRECATE keeps it from redefining the
// CRT's own names, which this tree still uses through its UStr* wrappers.
#define STRSAFE_NO_DEPRECATE
#include <StrSafe.h>

// Windows already defines most of this vocabulary (BOOL, INT, UINT, WCHAR,
// TCHAR, PTSTR, PCTSTR, PSTR, PWSTR, PCSTR, PCWSTR, HANDLE, VOID, TRUE, FALSE,
// NULL, MAX_PATH). These are the ones it does not.
#ifndef STATIC
#define STATIC	static
#endif

typedef unsigned __int64	UINT64;
// const void, which Windows also leaves to the caller to name.
typedef const VOID			*PCVOID;
typedef DWORD (WINAPI*		UPROC)(PVOID pvParam);

// The two buffer sizes this tree sizes things with. MAX_PATH is Windows' own.
#ifndef MAX_STR
#define MAX_STR		1024
#endif
#ifndef MAX_NAME
#define MAX_NAME	80
#endif

#define _NumOf(s)		(sizeof(s) / sizeof(s[0]))
// LoadString into the caller's buffer. The one-argument form is for a caller
// whose buffer is named tzStr, which is the name the call sites use for it.
#define _LoadStr(i, t)	(LoadString(g_hInst, i, t, _NumOf(t)), t)
#define _GetStr(i)		_LoadStr(i, tzStr)
// A resource id handed to the RT_* APIs, which take either a name or an id
// widened to a pointer. The cast goes through ULONG_PTR, so the id stays the
// same number on both architectures instead of being cut to 32 bits.
#define _MakeIntRes(i)	((PTSTR) (ULONG_PTR) (i))

// --- strings ---------------------------------------------------------------
// Wide only, because this build is Unicode-only. The lstr*/Str* calls are the
// Win32 ones: how long a string is on this ABI is what the system says it is.
inline UINT UStrLen(PCTSTR ptzStr)
{
	return (UINT) lstrlen(ptzStr);
}

inline PTSTR UStrCopy(PTSTR ptzDst, PCTSTR ptzSrc)
{
	return lstrcpy(ptzDst, ptzSrc);
}

inline PTSTR UStrCopyN(PTSTR ptzDst, PCTSTR ptzSrc, UINT uLen)
{
	return StrCpyNW(ptzDst, ptzSrc, (int) uLen);
}

inline PTSTR UStrCat(PTSTR ptzDst, PCTSTR ptzSrc)
{
	return lstrcat(ptzDst, ptzSrc);
}

inline INT UStrCmp(PCTSTR ptzStr1, PCTSTR ptzStr2)
{
	return lstrcmp(ptzStr1, ptzStr2);
}

inline INT UStrCmpI(PCTSTR ptzStr1, PCTSTR ptzStr2)
{
	return lstrcmpi(ptzStr1, ptzStr2);
}

inline INT UStrCmpN(PCTSTR ptzStr1, PCTSTR ptzStr2, UINT uLen)
{
	return StrCmpN(ptzStr1, ptzStr2, (int) uLen);
}

inline INT UStrCmpNI(PCTSTR ptzStr1, PCTSTR ptzStr2, UINT uLen)
{
	return StrCmpNI(ptzStr1, ptzStr2, (int) uLen);
}

inline PTSTR UStrChr(PCTSTR ptzStr, TCHAR c)
{
	return (PTSTR) StrChr(ptzStr, c);
}

inline PTSTR UStrRChr(PCTSTR ptzStr, TCHAR c)
{
	return (PTSTR) StrRChr(ptzStr, NULL, c);
}

inline PTSTR UStrStr(PCTSTR ptzStr1, PCTSTR ptzStr2)
{
	return (PTSTR) StrStr(ptzStr1, ptzStr2);
}

inline PTSTR UStrToUpper(PTSTR ptzStr)
{
	return (PTSTR) CharUpper(ptzStr);
}

inline INT UStrToInt(PCTSTR ptzStr)
{
	return (INT) _wtoi(ptzStr);
}

// Bounded formatting. wsprintf takes no destination size and writes whatever
// the format produces, so a call whose text can outgrow its buffer is a stack
// overflow -- and the alternative, wvsprintf, is the same function again. These
// take the array itself, truncate instead of running over, and answer with the
// number of characters that fitted, so a caller that appends at the returned
// offset stays inside the buffer.
//
// Passing a pointer rather than an array is a compile error on purpose: the
// size cannot be recovered from a pointer, so those few call sites name the
// size themselves with UStrPrintN.
template <size_t N>
inline int UStrPrintTo(TCHAR (&buf)[N], const TCHAR *ptzFmt, ...)
{
	va_list args;
	HRESULT hr;

	va_start(args, ptzFmt);
	hr = StringCchVPrintf(buf, N, ptzFmt, args);
	va_end(args);
	// StringCchVPrintf terminates the buffer on both paths, so its length is the
	// answer whichever way it went -- and it carries the SAL annotations that
	// tell the analyser so, which the CRT's own printf does not.
	return (int) UStrLen(buf);
}

template <size_t N>
inline int UStrPrintVTo(TCHAR (&buf)[N], const TCHAR *ptzFmt, va_list args)
{
	HRESULT hr = StringCchVPrintf(buf, N, ptzFmt, args);

	if (FAILED(hr))
	{
		buf[N - 1] = 0;		// the call already terminated it; this is belt and braces
	}
	return (int) UStrLen(buf);
}

#define UStrPrint(buf, ...)			UStrPrintTo(buf, __VA_ARGS__)
#define UStrPrintN(buf, n, ...)		StringCchPrintf(buf, (size_t) (n), __VA_ARGS__)
#define UStrPrintV(buf, fmt, va)	UStrPrintVTo(buf, fmt, va)

// Copies that refuse to cut, for the places where a shortened string names a
// different thing -- a path, a registry key, a command. The capacity comes from
// the array itself, so the check cannot drift away from the buffer; passing a
// pointer does not compile, on purpose.
template <size_t N>
inline BOOL UStrCopyTo(TCHAR (&dst)[N], PCTSTR src)
{
	return SUCCEEDED(StringCchCopy(dst, N, src));
}

template <size_t N>
inline BOOL UStrCatTo(TCHAR (&dst)[N], PCTSTR src)
{
	return SUCCEEDED(StringCchCat(dst, N, src));
}

// The narrow-string length, for the half of the code that works in bytes.
inline UINT UAStrLen(PCSTR pszStr)
{
	return (UINT) strlen(pszStr);
}

// And the explicit wide one, for the callers that name the width rather than
// using the TCHAR spelling.
inline UINT UWStrLen(PCWSTR pwzStr)
{
	return (UINT) lstrlen(pwzStr);
}

// The two cross-codepage conversions live in core/encoding.cpp; these are the
// names the call sites use for them.
PSTR UWStrToAStr(PSTR pszDst, PCWSTR pwzSrc, UINT uLen);
PWSTR UAStrToWStr(PWSTR pwzDst, PCSTR pszSrc, UINT uLen);
#define UStrToAStr(s, t, n)		UWStrToAStr(s, t, n)
#define UStrToWStr(w, t, n)		UStrCopyN(w, t, n)

// ASCII predicates: a digit, and "the same character, letter case ignored". The
// second one folds the two members of a case pair together and compares, which
// is all the case-insensitive comparison in the capability layer needs from it.
inline BOOL UChrIsNum(TCHAR c)
{
	return (c >= '0') && (c <= '9');
}

inline TCHAR UChrToUpper(TCHAR c)
{
	return ((c >= 'a') && (c <= 'z')) ? (TCHAR) (c + 'A' - 'a') : c;
}

inline BOOL UChrEqual(TCHAR c1, TCHAR c2)
{
	return UChrToUpper(c1) == UChrToUpper(c2);
}

// --- memory ----------------------------------------------------------------
// The C runtime heap, because that is what the callers pair this with: what is
// handed back here is handed to free() later.
inline PVOID UMemAlloc(UINT uSize)
{
	return malloc(uSize);
}

inline PVOID UMemRealloc(PVOID pvMem, UINT uSize)
{
	return realloc(pvMem, uSize);
}

inline VOID UMemFree(PVOID pvMem)
{
	free(pvMem);
}

inline PVOID UMemSet(PVOID pvMem, CHAR cVal, UINT uSize)
{
	return memset(pvMem, cVal, uSize);
}

inline PVOID UMemCopy(PVOID pvDst, PCVOID pvSrc, UINT uSize)
{
	return memcpy(pvDst, pvSrc, uSize);
}

// --- files -----------------------------------------------------------------
#define UFILE_READ		GENERIC_READ
#define UFILE_WRITE		GENERIC_WRITE
#define UFILE_APPEND	(GENERIC_READ | GENERIC_WRITE)

// The create rule follows the access mode: write starts a new file, append
// keeps what is there and leaves the pointer at the end of it, read wants the
// file to exist. A file that could not be opened answers NULL rather than
// INVALID_HANDLE_VALUE, which is what this vocabulary has always returned.
inline HANDLE UFileOpen(PCTSTR ptzPath, DWORD dwAccess)
{
	DWORD dwCreate = (dwAccess == UFILE_WRITE) ? CREATE_ALWAYS : ((dwAccess == UFILE_APPEND) ? OPEN_ALWAYS : OPEN_EXISTING);
	HANDLE hFile = CreateFile(ptzPath, dwAccess, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, dwCreate, 0, NULL);

	if (hFile == INVALID_HANDLE_VALUE)
	{
		return NULL;
	}
	if (dwAccess == UFILE_APPEND)
	{
		SetFilePointer(hFile, 0, NULL, FILE_END);
	}
	return hFile;
}

inline BOOL UFileClose(HANDLE hFile)
{
	return CloseHandle(hFile);
}

inline UINT UFileWrite(HANDLE hFile, PCVOID pvData, UINT uSize)
{
	DWORD dwWritten = 0;

	WriteFile(hFile, pvData, uSize, &dwWritten, NULL);
	return dwWritten;
}

inline BOOL UFileExist(PCTSTR ptzPath)
{
	HANDLE hFile = UFileOpen(ptzPath, UFILE_READ);

	if (hFile)
	{
		UFileClose(hFile);
		return TRUE;
	}
	return FALSE;
}

inline UINT UFileSave(PCTSTR ptzPath, PCVOID pvData, UINT uSize, DWORD dwAccess)
{
	HANDLE hFile = UFileOpen(ptzPath, dwAccess);
	UINT uWritten = 0;

	if (hFile)
	{
		uWritten = UFileWrite(hFile, pvData, uSize);
		UFileClose(hFile);
	}
	return uWritten;
}

// Read a whole file. A NULL buffer means "make me one", and the caller frees
// that with UMemFree; *puSize comes back as the number of bytes read either
// way. The two bytes written past the file's end are what keeps the result
// usable as text, and the allocation leaves room for them.
inline PVOID UFileLoad(PCTSTR ptzPath, PVOID pvData, PUINT puSize)
{
	HANDLE hFile = UFileOpen(ptzPath, UFILE_READ);

	if (hFile)
	{
		LARGE_INTEGER liSize;
		UINT uFileSize = 0;
		UINT uWant;

		if (!pvData)
		{
			// GetFileSizeEx, not GetFileSize with a NULL high word: the size
			// decides the allocation below, so the high half of it cannot be
			// dropped.
			if (GetFileSizeEx(hFile, &liSize) && (liSize.QuadPart > 0))
			{
				uFileSize = (liSize.QuadPart > (LONGLONG) UINT_MAX) ? UINT_MAX : (UINT) liSize.QuadPart;
			}
			pvData = UMemAlloc(uFileSize + 16);
			uWant = uFileSize;
		}
		else
		{
			// A caller's buffer is filled to two bytes short of its size, so
			// the two terminators written below stay inside it.
			UINT uHave = ((puSize != NULL) && (*puSize >= 2)) ? (*puSize - 2) : 0;

			uWant = (uHave < uFileSize) ? uHave : uFileSize;
		}
		if (pvData)
		{
			DWORD dwRead = 0;

			if (!ReadFile(hFile, pvData, uWant, &dwRead, NULL))
			{
				dwRead = 0;		// the terminators below still make it an empty string
			}
			// Terminate where the read ended rather than at the file's size:
			// with a caller's buffer the two are not the same, and using the
			// file's size wrote past the end of it.
			((PBYTE) pvData)[dwRead] = 0;
			((PBYTE) pvData)[dwRead + 1] = 0;
			if (puSize)
			{
				*puSize = (UINT) dwRead;
			}
		}
		UFileClose(hFile);
	}
	return pvData;
}

// --- directories -----------------------------------------------------------
inline BOOL UDirExist(PCTSTR ptzPath)
{
	WIN32_FILE_ATTRIBUTE_DATA fa;

	return GetFileAttributesEx(ptzPath, GetFileExInfoStandard, &fa) &&
		((fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
}

// Create the whole path, one backslash at a time. The walk is what makes
// "C:\A\B\C" work when only C: is there, and it stops at the last backslash
// because what is being created is a directory, not a file name.
inline BOOL UDirCreate(PTSTR ptzDir)
{
	PTSTR ptzSep;

	for (ptzSep = UStrChr(ptzDir, '\\'); ptzSep != NULL; ptzSep = UStrChr(ptzSep + 1, '\\'))
	{
		*ptzSep = 0;
		if (!UDirExist(ptzDir))
		{
			CreateDirectory(ptzDir, NULL);
		}
		*ptzSep = '\\';
	}
	// The walk reports nothing, and it cannot: the first component it stops at
	// is the drive itself ("X:"), where CreateDirectory's answer means nothing,
	// and the last component is never visited at all -- this walks the
	// separators, so what it creates is the parents of ptzDir. A caller that
	// needs the answer for the directory it asked for uses DirCreate().
	return TRUE;
}

// Split a path in place: everything up to the last backslash stays in ptzPath,
// and what is returned points at the name past it. With no backslash the whole
// string is the name, and it is returned unchanged.
inline PTSTR UDirSplitPath(PTSTR ptzPath)
{
	PTSTR ptzSep = UStrRChr(ptzPath, '\\');

	if (ptzSep)
	{
		*ptzSep = 0;
		return ptzSep + 1;
	}
	return ptzPath;
}

// --- threads ---------------------------------------------------------------
inline HANDLE UThreadCreate(UPROC upProc, PVOID pvParam)
{
	DWORD dwThread;

	return CreateThread(NULL, 0, upProc, pvParam, 0, &dwThread);
}

inline BOOL UThreadClose(HANDLE hThread)
{
	return CloseHandle(hThread);
}
