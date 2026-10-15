//
// File commands: READ, WRIT, SIZE, MDIR, FLNK.
//
//   READ <file>,<line>,<&var>   line, 0 = whole file, -1 = number of lines
//   WRIT <file>,<+n|-n|n|0>,[$]<text>
//   SIZE <&var>=<file or directory>
//   MDIR <directory>
//   FLNK <link>,<target>[,1]    hard link by default, 1 = symbolic link,
//                               -j = directory symbolic link, empty target
//                               removes the link
//
// READ and WRIT exist here in the shapes a script uses to read a line, write a
// line and ask how big something is.
//

//////////////////////////////////////////////////////////////////////////////////////////////////
// Text encoding: READ and WRIT's -ANSI / -GBK / -BIG5 / -UTF8 / -UNICODE flags.
//
// The default for both is the ANSI code page, plain, with no byte order mark -
// the flags exist to ask for something else, which is why WRIT is not
// "UTF-16 unless told otherwise". A BOM already in the file wins over the flag
// when reading.
//////////////////////////////////////////////////////////////////////////////////////////////////
struct TextEncoding
{
	UINT uCodePage;      // the 8-bit forms
	BOOL bUtf16;
	BOOL bUtf16Be;
	BOOL bBom;           // write a byte order mark
};

static BOOL ParseTextEncoding(PTSTR *pptzCmd, TextEncoding *pe)
{
	PTSTR ptzCmd = *pptzCmd;

	pe->uCodePage = CP_ACP;
	pe->bUtf16 = FALSE;
	pe->bUtf16Be = FALSE;
	pe->bBom = FALSE;

	while (ptzCmd[0] == '-')
	{
		PTSTR ptzNext = UStrChr(ptzCmd, ' ');

		if (!UStrCmpNI(ptzCmd, TEXT("-UNICODEB"), 9) || !UStrCmpNI(ptzCmd, TEXT("-UNIBE"), 6))
		{
			pe->bUtf16 = TRUE;
			pe->bUtf16Be = TRUE;
			pe->bBom = TRUE;
		}
		else if (!UStrCmpNI(ptzCmd, TEXT("-UNICODE"), 8) || !UStrCmpNI(ptzCmd, TEXT("-UNI"), 4))
		{
			pe->bUtf16 = TRUE;
			pe->bBom = TRUE;
		}
		else if (!UStrCmpNI(ptzCmd, TEXT("-UTF8"), 5))
		{
			pe->uCodePage = CP_UTF8;
			pe->bBom = TRUE;
		}
		else if (!UStrCmpNI(ptzCmd, TEXT("-GBK"), 4))
		{
			pe->uCodePage = 936;
		}
		else if (!UStrCmpNI(ptzCmd, TEXT("-BIG5"), 5))
		{
			pe->uCodePage = 950;
		}
		else if (!UStrCmpNI(ptzCmd, TEXT("-ANSI"), 5))
		{
			pe->uCodePage = CP_ACP;
		}
		else
		{
			INT iPage = UStrToInt(ptzCmd + 1);
			if (iPage <= 0)
			{
				return FALSE;
			}
			pe->uCodePage = (UINT) iPage;
		}
		if (!ptzNext)
		{
			break;
		}
		ptzCmd = UStrTrim(ptzNext + 1);
	}
	*pptzCmd = ptzCmd;
	return TRUE;
}

// File bytes -> wide text, freeing the buffer.
static PWSTR DecodeTextBuffer(PBYTE pbFile, UINT uSize, const TextEncoding &enc)
{
	PWSTR pwzOut;
	UINT uSkip = 0;
	BOOL bUtf16 = enc.bUtf16;
	BOOL bSwap = enc.bUtf16Be;
	UINT uCodePage = enc.uCodePage;

	if ((uSize >= 2) && (pbFile[0] == 0xFF) && (pbFile[1] == 0xFE))
	{
		bUtf16 = TRUE;
		bSwap = FALSE;
		uSkip = 2;
	}
	else if ((uSize >= 2) && (pbFile[0] == 0xFE) && (pbFile[1] == 0xFF))
	{
		bUtf16 = TRUE;
		bSwap = TRUE;
		uSkip = 2;
	}
	else if ((uSize >= 3) && (pbFile[0] == 0xEF) && (pbFile[1] == 0xBB) && (pbFile[2] == 0xBF))
	{
		uCodePage = CP_UTF8;
		uSkip = 3;
	}

	if (bUtf16)
	{
		UINT uChars = (uSize - uSkip) / 2;
		UINT i;
		pwzOut = (PWSTR) UMemAlloc((uChars + 1) * sizeof(WCHAR));
		for (i = 0; i < uChars; i++)
		{
			UINT uAt = uSkip + i * 2;
			pwzOut[i] = bSwap
				? (WCHAR) ((pbFile[uAt] << 8) | pbFile[uAt + 1])
				: (WCHAR) (pbFile[uAt] | (pbFile[uAt + 1] << 8));
		}
		pwzOut[uChars] = 0;
	}
	else
	{
		UINT uChars = (uSize - uSkip) + 1;
		INT n;
		pwzOut = (PWSTR) UMemAlloc((uChars + 1) * sizeof(WCHAR));
		n = MultiByteToWideChar(uCodePage, 0, (PCSTR) (pbFile + uSkip),
			(INT) (uSize - uSkip), pwzOut, (INT) uChars);
		pwzOut[(n < 0) ? 0 : n] = 0;
	}
	UMemFree(pbFile);
	return pwzOut;
}

// Wide text -> file bytes. The caller frees the result; *puBytes is its length.
static PBYTE EncodeTextBuffer(PCTSTR ptzText, const TextEncoding &enc, UINT *puBytes)
{
	UINT uChars = UStrLen(ptzText);
	PBYTE pbOut;

	if (enc.bUtf16)
	{
		UINT uAt = 0;
		UINT i;
		*puBytes = (enc.bBom ? 2 : 0) + uChars * 2;
		pbOut = (PBYTE) UMemAlloc(*puBytes + 1);
		if (!pbOut)
		{
			return NULL;
		}
		if (enc.bBom)
		{
			pbOut[0] = enc.bUtf16Be ? 0xFE : 0xFF;
			pbOut[1] = enc.bUtf16Be ? 0xFF : 0xFE;
			uAt = 2;
		}
		for (i = 0; i < uChars; i++)
		{
			WCHAR c = ptzText[i];
			if (enc.bUtf16Be)
			{
				pbOut[uAt++] = (BYTE) (c >> 8);
				pbOut[uAt++] = (BYTE) (c & 0xFF);
			}
			else
			{
				pbOut[uAt++] = (BYTE) (c & 0xFF);
				pbOut[uAt++] = (BYTE) (c >> 8);
			}
		}
		return pbOut;
	}

	{
		INT n = WideCharToMultiByte(enc.uCodePage, 0, ptzText, (INT) uChars, NULL, 0, NULL, NULL);
		UINT uBody = (n > 0) ? (UINT) n : 0;
		UINT uAt = 0;
		*puBytes = (enc.bBom ? 3 : 0) + uBody;
		pbOut = (PBYTE) UMemAlloc(*puBytes + 1);
		if (!pbOut)
		{
			return NULL;
		}
		if (enc.bBom)
		{
			pbOut[0] = 0xEF;
			pbOut[1] = 0xBB;
			pbOut[2] = 0xBF;
			uAt = 3;
		}
		if (uBody)
		{
			WideCharToMultiByte(enc.uCodePage, 0, ptzText, (INT) uChars, (PSTR) (pbOut + uAt), (INT) uBody, NULL, NULL);
		}
	}
	return pbOut;
}

// The size of a file, or of everything under a directory.
static BOOL PathSizeBytes(PCTSTR ptzPath, UINT64 *puSize)
{
	WIN32_FILE_ATTRIBUTE_DATA fa;
	WIN32_FIND_DATA fd;
	HANDLE hFind;
	TCHAR tzPattern[MAX_PATH];
	UINT64 uTotal = 0;

	if (!GetFileAttributesEx(ptzPath, GetFileExInfoStandard, &fa))
	{
		return FALSE;
	}
	if (!(fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
	{
		*puSize = ((UINT64) fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
		return TRUE;
	}

	// A path that cannot fit is not measured: the caller named something this
	// buffer cannot spell.
	if ((UStrLen(ptzPath) + 3) >= _NumOf(tzPattern))
	{
		return FALSE;
	}
	UStrPrint(tzPattern, TEXT("%s\\*"), ptzPath);
	hFind = FindFirstFile(tzPattern, &fd);
	if (hFind == INVALID_HANDLE_VALUE)
	{
		*puSize = 0;
		return TRUE;                   // an empty directory is size zero
	}
	do
	{
		TCHAR tzChild[MAX_PATH];
		if ((UStrCmp(fd.cFileName, TEXT(".")) == 0) || (UStrCmp(fd.cFileName, TEXT("..")) == 0))
		{
			continue;
		}
		if ((UStrLen(ptzPath) + UStrLen(fd.cFileName) + 2) >= _NumOf(tzChild))
		{
			continue;		// the same: a name that does not fit is not that file
		}
		UStrPrint(tzChild, TEXT("%s\\%s"), ptzPath, fd.cFileName);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			UINT64 uChild = 0;
			if (PathSizeBytes(tzChild, &uChild))
			{
				uTotal += uChild;
			}
		}
		else
		{
			uTotal += ((UINT64) fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
		}
	}
	while (FindNextFile(hFind, &fd));
	FindClose(hFind);

	*puSize = uTotal;
	return TRUE;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// SIZE
//////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT Size(CoreContext& ctx, PTSTR ptzCmd)
{
	PTSTR ptzName = NULL;
	PTSTR ptzPath;
	UINT64 uSize = 0;
	TCHAR tzNumber[32];

	// -link follows reparse points, which the recursive walk does not do; it is
	// recognised and skipped because the answer is the same for a plain file.
	while (!UStrCmpNI(ptzCmd, TEXT("-link"), 5))
	{
		ptzCmd = UStrTrim(ptzCmd + 5);
	}
	ptzPath = SplitNameValue(ptzCmd, &ptzName);
	if (!ptzName || !ptzPath)
	{
		return E_INVALIDARG;
	}
	ptzPath = UStrTrim(ptzPath);
	if (!PathSizeBytes(ptzPath, &uSize))
	{
		return S_FALSE;
	}
	_snwprintf_s(tzNumber, _NumOf(tzNumber), _TRUNCATE, TEXT("%I64u"), uSize);
	return StoreResult(ptzName, tzNumber) ? S_OK : S_FALSE;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// MDIR
//////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT Mkdir(CoreContext& ctx, PTSTR ptzCmd)
{
	TCHAR tzPath[MAX_PATH];

	if (!UStrCopyTo(tzPath, UStrTrim(ptzCmd)))
	{
		return E_INVALIDARG;
	}
	if (!tzPath[0])
	{
		return E_INVALIDARG;
	}
	// UDirCreate makes every missing parent on the way, which is what the
	// command is for.
	return UDirCreate(tzPath) || UDirExist(tzPath) ? S_OK : S_FALSE;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// READ
//////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT Read(CoreContext& ctx, PTSTR ptzCmd)
{
	TextEncoding enc;
	PTSTR ptzPath;
	PTSTR ptzWhich;
	PTSTR ptzVar;
	INT iLine;
	UINT uSize = (UINT) -1;
	PBYTE pbFile;
	PWSTR pwzText;
	UINT uLines = 1;
	TCHAR tzOut[MAX_STR];
	UINT i;
	UINT uStart = 0;
	UINT uEnd = 0;
	UINT uWant;

	if (!ParseTextEncoding(&ptzCmd, &enc))
	{
		return E_INVALIDARG;
	}

	// <file>,<line>,<&var>
	ptzWhich = UStrChr(ptzCmd, ',');
	if (!ptzWhich)
	{
		return E_INVALIDARG;
	}
	*ptzWhich++ = 0;
	ptzVar = UStrChr(ptzWhich, ',');
	if (!ptzVar)
	{
		return E_INVALIDARG;
	}
	*ptzVar++ = 0;
	ptzPath = UStrTrim(ptzCmd);
	iLine = UStrToInt(UStrTrim(ptzWhich));
	ptzVar = UStrTrim(ptzVar);
	if (!ptzPath[0] || !ptzVar[0])
	{
		return E_INVALIDARG;
	}

	pbFile = (PBYTE) UFileLoad(ptzPath, NULL, &uSize);
	if (!pbFile)
	{
		// A file that is not there comes back as the Win32 error, the way the
		// text form reports it: 2 (ERROR_FILE_NOT_FOUND), not a bare failure.
		return (HRESULT) GetLastError();
	}
	pwzText = DecodeTextBuffer(pbFile, uSize, enc);

	for (i = 0; pwzText[i]; i++)
	{
		if (pwzText[i] == '\n')
		{
			uLines++;
		}
	}
	// A file that ends with a newline does not have an empty line after it.
	if ((i > 0) && (pwzText[i - 1] == '\n'))
	{
		uLines--;
	}

	if (iLine < 0)
	{
		UStrPrint(tzOut, TEXT("%u"), uLines);
		UMemFree(pwzText);
		return StoreResult(ptzVar, tzOut) ? S_OK : S_FALSE;
	}
	if (iLine == 0)
	{
		UStrCopyN(tzOut, pwzText, _NumOf(tzOut));
		UMemFree(pwzText);
		return StoreResult(ptzVar, tzOut) ? S_OK : S_FALSE;
	}

	// Walk to the wanted line. The value keeps the carriage return and drops
	// only the line feed - measured against the text form: reading "first\r\n"
	// back answers "first\r", not "first".
	uWant = (UINT) iLine;
	{
		UINT uLine = 1;
		for (i = 0; pwzText[i] && (uLine < uWant); i++)
		{
			if (pwzText[i] == '\n')
			{
				uLine++;
				uStart = i + 1;
			}
		}
		if (uLine < uWant)
		{
			UMemFree(pwzText);
			return StoreResult(ptzVar, TEXT("")) ? S_OK : S_FALSE;
		}
		for (uEnd = uStart; pwzText[uEnd] && (pwzText[uEnd] != '\n'); uEnd++)
		{
		}
	}
	i = uEnd - uStart;
	if (i + 1 > _NumOf(tzOut))
	{
		i = _NumOf(tzOut) - 1;
	}
	UMemCopy(tzOut, pwzText + uStart, i * sizeof(TCHAR));
	tzOut[i] = 0;
	UMemFree(pwzText);
	return StoreResult(ptzVar, tzOut) ? S_OK : S_FALSE;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// WRIT
//////////////////////////////////////////////////////////////////////////////////////////////////
// What the line number means: "+n" inserts a new line before line n, "-n"
// deletes line n, and a plain n replaces it. Line numbers are 1-based, and 0
// means the last line (for "+0", one past it: append).
enum WritMode { WRIT_REPLACE = 0, WRIT_INSERT, WRIT_DELETE };

// Split a text file into lines, run the caller's change, and write it back.
// Returns S_FALSE when the file cannot be read or written.
HRESULT Writ(CoreContext& ctx, PTSTR ptzCmd)
{
	TextEncoding enc;
	BOOL bExpand = FALSE;
	PTSTR ptzPath;
	PTSTR ptzLine;
	PTSTR ptzText;
	INT iLine = 0;
	INT iMode = WRIT_REPLACE;
	UINT uSize = (UINT) -1;
	PBYTE pbFile = NULL;
	PWSTR pwzText = NULL;
	TCHAR tzPath[MAX_PATH];
	// Line table: where each line starts and ends in the wide text.
	// On the heap: the two tables together are eight kilobytes, which is half of
	// what /analyze will accept in one frame, and the line count is bounded by
	// their size rather than by the stack.
	std::vector<UINT> uStarts(MAX_STR);
	std::vector<UINT> uEnds(MAX_STR);
	UINT uCount = 0;
	UINT i;

	if (!ParseTextEncoding(&ptzCmd, &enc))
	{
		return E_INVALIDARG;
	}
	ptzLine = UStrChr(ptzCmd, ',');
	if (!ptzLine)
	{
		return E_INVALIDARG;
	}
	*ptzLine++ = 0;
	ptzText = UStrChr(ptzLine, ',');
	if (ptzText)
	{
		*ptzText++ = 0;
	}
	ptzPath = UStrTrim(ptzCmd);
	ptzLine = UStrTrim(ptzLine);

	if (*ptzLine == '$')
	{
		bExpand = TRUE;
		ptzLine++;
	}
	if (*ptzLine == '+')
	{
		iMode = WRIT_INSERT;
		ptzLine++;
	}
	else if (*ptzLine == '-')
	{
		iMode = WRIT_DELETE;
		ptzLine++;
	}
	iLine = UStrToInt(UStrTrim(ptzLine));

	if (!ptzPath[0])
	{
		return E_INVALIDARG;
	}
	if (!UStrCopyTo(tzPath, ptzPath))
	{
		return E_INVALIDARG;
	}

	if (UFileExist(tzPath))
	{
		pbFile = (PBYTE) UFileLoad(tzPath, NULL, &uSize);
		if (!pbFile)
		{
			return S_FALSE;
		}
		pwzText = DecodeScript(pbFile, uSize);
	}
	else
	{
		pwzText = (PWSTR) UMemAlloc(sizeof(TCHAR));
		pwzText[0] = 0;
	}

	// Split into lines, without the terminators.
	for (i = 0; pwzText[i] && (uCount < uStarts.size()); )
	{
		UINT uStart = i;
		while (pwzText[i] && (pwzText[i] != '\r') && (pwzText[i] != '\n'))
		{
			i++;
		}
		uStarts[uCount] = uStart;
		uEnds[uCount] = i;
		uCount++;
		while ((pwzText[i] == '\r') || (pwzText[i] == '\n'))
		{
			i++;
		}
	}

	{
		TCHAR tzOut[MAX_STR * 4];
		UINT uOut = 0;
		TCHAR tzNew[MAX_STR];
		UINT uInsert = 0;

		if (ptzText && bExpand)
		{
			// Expanded into the buffer, from another one: the API does not
			// promise anything when the source and the destination are the same
			// block.
			TCHAR tzRaw[MAX_STR];

			if (!UStrCopyTo(tzRaw, ptzText) ||
				(ExpandEnvironmentStrings(tzRaw, tzNew, _NumOf(tzNew)) == 0))
			{
				return E_INVALIDARG;
			}
		}
		else if (ptzText)
		{
			if (!UStrCopyTo(tzNew, ptzText))
			{
				return E_INVALIDARG;
			}
		}
		else
		{
			tzNew[0] = 0;
		}

		// 1-based line number -> 0-based index; 0 or past the end means the end
		// of the file, which for an insert is where a new line is appended.
		if (iLine < 1)
		{
			uInsert = uCount;
		}
		else
		{
			uInsert = (UINT) (iLine - 1);
			if (uInsert > uCount)
			{
				uInsert = uCount;
			}
		}

		for (i = 0; (i < uCount) && (uOut + 4 < _NumOf(tzOut)); i++)
		{
			if ((iMode == WRIT_DELETE) && (i == uInsert))
			{
				continue;              // drop that line
			}
			if ((iMode != WRIT_DELETE) && ptzText && (i == uInsert))
			{
				UINT k;
				for (k = 0; tzNew[k] && (uOut + 4 < _NumOf(tzOut)); k++)
				{
					tzOut[uOut++] = tzNew[k];
				}
				tzOut[uOut++] = '\r';
				tzOut[uOut++] = '\n';
				if (iMode == WRIT_REPLACE)
				{
					continue;          // the new line took this one's place
				}
			}
			{
				UINT k;
				for (k = uStarts[i]; (k < uEnds[i]) && (uOut + 4 < _NumOf(tzOut)); k++)
				{
					tzOut[uOut++] = pwzText[k];
				}
				tzOut[uOut++] = '\r';
				tzOut[uOut++] = '\n';
			}
		}
		// A line past the last one: an insert appends, and so does a replace of a
		// line the file does not have.
		if ((iMode != WRIT_DELETE) && ptzText && (uInsert >= uCount))
		{
			UINT k;
			for (k = 0; tzNew[k] && (uOut + 4 < _NumOf(tzOut)); k++)
			{
				tzOut[uOut++] = tzNew[k];
			}
			tzOut[uOut++] = '\r';
			tzOut[uOut++] = '\n';
		}
		tzOut[uOut] = 0;

		{
			UINT uBytes = 0;
			PBYTE pbOut = EncodeTextBuffer(tzOut, enc, &uBytes);
			if (!pbOut || (UFileSave(tzPath, pbOut, uBytes, UFILE_WRITE) != uBytes))
			{
				UMemFree(pbOut);
				UMemFree(pwzText);
				return S_FALSE;
			}
			UMemFree(pbOut);
		}
	}

	UMemFree(pwzText);
	return S_OK;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// FLNK
//////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT Flnk(CoreContext& ctx, PTSTR ptzCmd)
{
	BOOL bJunction = FALSE;
	BOOL bSymlink = FALSE;
	PTSTR ptzSource;
	PTSTR ptzTarget;
	PTSTR ptzType;

	if (!UStrCmpNI(ptzCmd, TEXT("-j"), 2))
	{
		bJunction = TRUE;
		ptzCmd = UStrTrim(ptzCmd + 2);
	}
	else if (ptzCmd[0] == '-')
	{
		return E_INVALIDARG;
	}

	ptzSource = UStrChr(ptzCmd, ',');
	if (!ptzSource)
	{
		return E_INVALIDARG;
	}
	*ptzSource++ = 0;
	ptzTarget = UStrTrim(ptzCmd);

	ptzType = UStrChr(ptzSource, ',');
	if (ptzType)
	{
		*ptzType++ = 0;
		if (*UStrTrim(ptzType) == '1')
		{
			bSymlink = TRUE;
		}
	}
	ptzSource = UStrTrim(ptzSource);

	if (!ptzTarget[0])
	{
		return E_INVALIDARG;
	}
	if (!ptzSource[0])
	{
		// An empty source deletes the link.
		if (UDirExist(ptzTarget))
		{
			return RemoveDirectory(ptzTarget) ? S_OK : S_FALSE;
		}
		return DeleteFile(ptzTarget) ? S_OK : S_FALSE;
	}

	if (bSymlink || bJunction)
	{
		// CreateSymbolicLink is Vista and later, and the project is pinned to a
		// pre-Vista _WIN32_WINNT, so it is looked up rather than linked.
		typedef BOOLEAN (WINAPI *PCSL)(PCWSTR, PCWSTR, DWORD);
		PCSL pfCreate;
		{
			HMODULE hKernel = GetModuleHandle(TEXT("KERNEL32"));

			pfCreate = hKernel ? (PCSL) GetProcAddress(hKernel, "CreateSymbolicLinkW") : NULL;
		}
#ifndef SYMBOLIC_LINK_FLAG_DIRECTORY
#define SYMBOLIC_LINK_FLAG_DIRECTORY 0x2
#endif
		if (!pfCreate)
		{
			return S_FALSE;
		}
		return pfCreate(ptzTarget, ptzSource, bJunction ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0) ? S_OK : S_FALSE;
	}
	return CreateHardLink(ptzTarget, ptzSource, NULL) ? S_OK : S_FALSE;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// DIR
//////////////////////////////////////////////////////////////////////////////////////////////////
// DIR <&var> [<mask or directory>]
//
// One name per line, which is what "the /b switch is implied" means. The other
// switches the DOS command has (/s, /a, the sort orders) each change what the
// list contains and in what order, so they are refused until the observed
// listing has been measured.
HRESULT Dir(CoreContext& ctx, PTSTR ptzCmd)
{
	PTSTR ptzVar;
	PTSTR ptzMask;
	TCHAR tzMask[MAX_PATH];
	// On the heap, for the same reason as the line tables in Writ.
	std::vector<TCHAR> tzOut(MAX_STR * 8);
	WIN32_FIND_DATA fd;
	HANDLE hFind;
	UINT uOut = 0;

	ptzMask = UStrChr(ptzCmd, ' ');
	if (!ptzMask)
	{
		return E_INVALIDARG;
	}
	*ptzMask++ = 0;
	ptzVar = UStrTrim(ptzCmd);
	ptzMask = UStrTrim(ptzMask);
	if (!ptzVar[0])
	{
		return E_INVALIDARG;
	}
	if ((ptzMask[0] == '/') || (ptzMask[0] == '-'))
	{
		return E_INVALIDARG;
	}
	if (!ptzMask[0])
	{
		UStrCopyTo(tzMask, TEXT("*"));
	}
	else if (UDirExist(ptzMask))
	{
		if ((UStrLen(ptzMask) + 3) >= _NumOf(tzMask))
		{
			return E_INVALIDARG;
		}
		UStrPrint(tzMask, TEXT("%s\\*"), ptzMask);
	}
	else
	{
		if (!UStrCopyTo(tzMask, ptzMask))
		{
			return E_INVALIDARG;
		}
	}

	tzOut[0] = 0;
	hFind = FindFirstFile(tzMask, &fd);
	if (hFind == INVALID_HANDLE_VALUE)
	{
		return S_FALSE;
	}
	do
	{
		UINT uLen;
		if ((UStrCmp(fd.cFileName, TEXT(".")) == 0) || (UStrCmp(fd.cFileName, TEXT("..")) == 0))
		{
			continue;
		}
		uLen = UStrLen(fd.cFileName);
		if (uOut + uLen + 4 >= tzOut.size())
		{
			break;
		}
		UStrCopy(&tzOut[uOut], fd.cFileName);
		uOut += uLen;
		tzOut[uOut++] = '\n';
		tzOut[uOut] = 0;
	}
	while (FindNextFile(hFind, &fd));
	FindClose(hFind);
	return StoreResult(ptzVar, &tzOut[0]) ? S_OK : S_FALSE;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// SITE
//////////////////////////////////////////////////////////////////////////////////////////////////
// SITE <+R|-H|+R-H>,<path>   set or clear file attributes
//
// A = archive, H = hidden, R = read-only, S = system, and the +/- in front of
// each letter says whether it is being set or cleared. The path is written
// first in one form ("SITE C:\BOOT.INI,+H+R") and the attributes first in the
// other (the order in the line above), so both orders are accepted.
//
// The query forms are not here: what the text form hands back for an attribute
// string is not documented well enough to reproduce without guessing, and a
// guess would be a silent difference.
HRESULT Site(CoreContext& ctx, PTSTR ptzCmd)
{
	PTSTR ptzAttrs;
	PTSTR ptzComma;
	TCHAR tzPath[MAX_PATH];
	TCHAR tzAttrs[MAX_NAME];
	DWORD dwAttrs;
	BOOL bSet = FALSE;
	BOOL bAny = FALSE;

	ptzComma = UStrChr(ptzCmd, ',');
	if (!ptzComma)
	{
		return E_INVALIDARG;
	}
	*ptzComma++ = 0;
	// The path comes first: "SITE C:\BOOT.INI,+H+R". Handed the attributes
	// first, the text form looks for a file called "+H" and answers 2, and the
	// case pins that so the two stay the same shape.
	if (!UStrCopyTo(tzPath, UStrTrim(ptzCmd)) || !UStrCopyTo(tzAttrs, UStrTrim(ptzComma)))
	{
		return E_INVALIDARG;
	}
	if (!tzPath[0])
	{
		return E_INVALIDARG;
	}

	// The file is looked up before the attributes are read. Handed the fields in
	// the wrong order the text form treats "+H" as the path, fails to find it,
	// and answers 2 - so the lookup has to come first for the two to agree.
	dwAttrs = GetFileAttributes(tzPath);
	if (dwAttrs == INVALID_FILE_ATTRIBUTES)
	{
		return (HRESULT) GetLastError();
	}
	for (ptzAttrs = tzAttrs; *ptzAttrs; ptzAttrs++)
	{
		DWORD dwFlag = 0;
		if (*ptzAttrs == '+')
		{
			bSet = TRUE;
			continue;
		}
		if (*ptzAttrs == '-')
		{
			bSet = FALSE;
			continue;
		}
		if ((*ptzAttrs == 'a') || (*ptzAttrs == 'A'))
		{
			dwFlag = FILE_ATTRIBUTE_ARCHIVE;
		}
		else if ((*ptzAttrs == 'h') || (*ptzAttrs == 'H'))
		{
			dwFlag = FILE_ATTRIBUTE_HIDDEN;
		}
		else if ((*ptzAttrs == 'r') || (*ptzAttrs == 'R'))
		{
			dwFlag = FILE_ATTRIBUTE_READONLY;
		}
		else if ((*ptzAttrs == 's') || (*ptzAttrs == 'S'))
		{
			dwFlag = FILE_ATTRIBUTE_SYSTEM;
		}
		else
		{
			return E_INVALIDARG;
		}
		dwAttrs = bSet ? (dwAttrs | dwFlag) : (dwAttrs & ~dwFlag);
		bAny = TRUE;
	}
	if (!bAny)
	{
		return E_INVALIDARG;
	}
	return SetFileAttributes(tzPath, dwAttrs) ? S_OK : S_FALSE;
}
