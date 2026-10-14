// One line of the script buffer, with any trailing backtick comment removed. The
// caller hands in the character that ends the line so this can stop there without
// copying the whole file.
PTSTR GetNewLine(PTSTR ptBuf, PTSTR ptOldLine, PTSTR ptEnd)
{
	TCHAR cSaved = *ptEnd;
	PTSTR ptzComment;

	*ptEnd = 0;
	UStrCopy(ptBuf, ptOldLine);
	*ptEnd = cSaved;

	ptzComment = UStrChr(ptBuf, '`');
	if (ptzComment)
	{
		*ptzComment = 0;
	}
	return UStrTrim(ptBuf);
}
//////////////////////////////////////////////////////////////////////////////////////////


////////////////////////////////////////////////////////////////////////////////////////////////////////
#define SUBLabel TEXT("_SUB")
#define ENDLabel TEXT("_END")
HRESULT Command(PTSTR);
//////////////////////////////////////////////////////////////////////////////////////////////////
// Script IR
//
// The file is parsed into a flat instruction list, and that list is what runs,
// so there is one place that understands script syntax and one place that
// executes it. The parse rules are the text form's, verbatim, quirks included:
//   * a line is cut at the first backtick
//   * lines starting with / or ` are skipped
//   * _SUB / _END are recognised by their first four characters
//
// "A trailing line without a terminator is never executed" is not on that list.
// The text form runs a last line that has no newline, and boot configurations
// rely on it: two files in a shipped configuration end without a newline, and
// dropping their last line loses Initapp.ini's last CALL and wallpaper.ini's
// only FORX. A file whose last byte is not a newline still has that last line.
//////////////////////////////////////////////////////////////////////////////////////////////////
enum InsnKind { INSN_COMMAND = 0, INSN_SUB, INSN_END };

struct Insn
{
	InsnKind kind;
	PTSTR         text;
	UINT          offset;      // offset of the line start inside the buffer
	UINT          line;        // 1-based source line, for the trace
};

struct InsnList
{
	PTSTR       buffer;
	Insn  *insns;
	UINT        count;
	UINT        capacity;
};

static InsnList *g_pScript = NULL;

static VOID ScriptFree(InsnList *script)
{
	if (script->insns)
	{
		for (UINT i = 0; i < script->count; i++)
		{
			UMemFree(script->insns[i].text);
		}
		UMemFree(script->insns);
	}
	script->buffer = NULL;
	script->insns = NULL;
	script->count = 0;
	script->capacity = 0;
}

static BOOL ScriptAdd(InsnList *script, InsnKind kind, PCTSTR text, UINT offset, UINT line)
{
	if (script->count == script->capacity)
	{
		UINT capacity = script->capacity ? script->capacity * 2 : 64;
		PVOID grown = UMemRealloc(script->insns, capacity * sizeof(Insn));
		if (!grown)
		{
			return FALSE;
		}
		script->insns = (Insn *) grown;
		script->capacity = capacity;
	}

	PTSTR copy = (PTSTR) UMemAlloc(MAX_STR * sizeof(TCHAR));
	if (!copy)
	{
		return FALSE;
	}
	copy[0] = 0;
	UStrCopyN(copy, (PTSTR) text, MAX_STR - 1);

	script->insns[script->count].kind = kind;
	script->insns[script->count].text = copy;
	script->insns[script->count].offset = offset;
	script->insns[script->count].line = line;
	script->count++;
	return TRUE;
}

static HRESULT ScriptParse(PTSTR buffer, InsnList *script)
{
	script->buffer = buffer;
	script->insns = NULL;
	script->count = 0;
	script->capacity = 0;

	// Line numbers are counted by walking the buffer forwards, so a script with
	// blank lines still numbers the way the text form implementation does.
	UINT uLine = 1;
	PTSTR pScanned = buffer;

	for (PTSTR p = buffer; *p; )
	{
		PTSTR q = p;
		while (*q && (*q != '\r') && (*q != '\n'))
		{
			q++;
		}

		TCHAR CurLine[MAX_STR];
		PTSTR Newp = GetNewLine(CurLine, p, q);

		while (pScanned < p)
		{
			if (*pScanned == '\n')
			{
				uLine++;
			}
			pScanned++;
		}

		if ((Newp[0]) && (Newp[0] != '/') && (Newp[0] != '`'))
		{
			InsnKind kind = INSN_COMMAND;
			if (!UStrCmpNI(Newp, SUBLabel, 4))
			{
				kind = INSN_SUB;
			}
			else if (!UStrCmpNI(Newp, ENDLabel, 4))
			{
				kind = INSN_END;
			}
			if (!ScriptAdd(script, kind, Newp, (UINT)(p - buffer), uLine))
			{
				ScriptFree(script);
				return E_OUTOFMEMORY;
			}
		}

		p = q;
		while ((*p == '\r') || (*p == '\n'))
		{
			p++;
		}
	}
	return S_OK;
}

// Top level: run commands that are not inside a _SUB ... _END block.
static VOID ScriptRunTop(InsnList *script)
{
	// Depth, not a flag: a _SUB nested inside a sub has its own _END, and a flag
	// would treat that _END as the end of the outer block and start running the
	// rest of the outer body at top level.
	INT uSubDepth = 0;
	for (UINT i = 0; i < script->count; i++)
	{
		if (script->insns[i].kind == INSN_SUB)
		{
			uSubDepth++;
		}
		else if (script->insns[i].kind == INSN_END)
		{
			if (uSubDepth > 0)
			{
				uSubDepth--;
			}
		}
		else if (uSubDepth == 0)
		{
				g_ctx.uLineNo = script->insns[i].line;
			Command(script->insns[i].text);
		}
	}
}

// Sub process: run from `start` until the next _END, exactly like the
// original line scanner (which also skipped any nested _SUB line).
static VOID ScriptRunFrom(InsnList *script, UINT start)
{
	for (UINT i = start; i < script->count; i++)
	{
		if (script->insns[i].kind == INSN_END)
		{
			break;
		}
		if (script->insns[i].kind == INSN_SUB)
		{
			if (i == start)
			{
				// i == start is this sub's own definition line, so run its body; only a
				// _SUB *inside* the body is skipped whole.
				continue;
			}
			// A _SUB nested inside a sub is skipped whole, and its _END does not end this
			// sub: the text form continues the outer body past the nested block. Running
			// the nested body and stopping at its _END reordered everything after it.
			UINT j = i + 1;
			while ((j < script->count) && (script->insns[j].kind != INSN_END))
			{
				j++;
			}
			i = j;
			continue;
		}
		g_ctx.uLineNo = script->insns[i].line;
		Command(script->insns[i].text);
	}
}

// Run the sub body whose name is ptzCmd.
HRESULT CallSub(PTSTR ptzCmd)
{
	TCHAR tzPath[MAX_NAME];
	// A sub body is a scope: a variable it creates locally is gone on return.
	PeVar::ScopeGuard scope;
	UStrPrint(tzPath, TEXT("%s %s"), SUBLabel,ptzCmd);

	PTSTR p = UStrStr(pFileStart, tzPath); // where the sub body starts
	if (p)
	{
		InsnList *script = g_pScript;
		if (script && (script->buffer == pFileStart))
		{
			UINT offset = (UINT)(p - pFileStart);
			for (UINT i = 0; i < script->count; i++)
			{
				if (script->insns[i].offset == offset)
				{
					ScriptRunFrom(script, i);
					return S_OK;
				}
			}
		}

		// The match did not land on an instruction boundary: it can sit inside
		// a comment or in the middle of a line. Fall back to the original line
		// scan so those cases keep their exact behaviour.
		for (PTSTR q = p; *q; q++)
		{
			if ((*q == '\r') || (*q == '\n'))
			{
				TCHAR CurLine[MAX_STR];
				PTSTR Newp =GetNewLine(CurLine,p,q);

				if ((Newp[0]) && (Newp[0] != '`') && (Newp[0] != '/')) // not a comment and not a blank line
				{
					if (UStrCmpNI(Newp, ENDLabel,4)) // not the _END that closes the sub
					{
						if (UStrCmpNI(Newp,SUBLabel,4)) // not the _SUB that opened it
						{
							Command(Newp); // Command() does not interpret _SUB or _END itself
						}
					}
					else
					{
						break;          // the sub body ends here
					}
				}
				for (q++; (*q == '\r') || (*q == '\n'); q++);
				p = q;
			}
		}
		return S_OK;
	}
	else
	{
		return S_FALSE;
	}
}
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
HRESULT Call(CoreContext& ctx, PTSTR ptzCmd)
{
	// "CALL $<dll>,<entry>" is a DLL entry point; anything else names a script
	// subprocedure.
	if (*ptzCmd == '$')
	{
		return CallDll(ptzCmd + 1);
	}
	return CallSub(ptzCmd);
}
/////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// Script encoding
//
// A script can say what it is written in: a byte-order mark, or a
// "#code=<number>" first line. With neither it decides for itself, and the rule
// a boot configuration depends on is "valid UTF-8 is UTF-8, anything else is
// the ANSI code page". That is not a nicety. One PE contains both without any
// BOM and without any directive - its own Windows\Config\LINK.ini is UTF-8,
// while System32\disposition\Post\Link.ini is GBK - and guessing wrong does not
// look broken: it produces *different Chinese characters*, which read as
// plausible until someone looks at the text: the same bytes read two ways give
// different characters, and both readings look plausible.
//////////////////////////////////////////////////////////////////////////////////////////////////

// A "#!" line comes first if there is one, then the directive. Returns the code
// page, and in *puLineEnd the offset just past the directive line, or 0.
static UINT ScriptCodeDirective(const BYTE *pb, UINT uLen, UINT *puLineEnd)
{
	UINT uStart = 0;
	UINT uValue = 0;
	UINT i;

	*puLineEnd = 0;
	if ((uLen >= 2) && (pb[0] == '#') && (pb[1] == '!'))
	{
		while ((uStart < uLen) && (pb[uStart] != '\n'))
		{
			uStart++;
		}
		if (uStart < uLen)
		{
			uStart++;
		}
	}
	if ((uLen < uStart + 6) || (memcmp(pb + uStart, "#code=", 6) != 0))
	{
		return 0;
	}
	for (i = uStart + 6; (i < uLen) && (pb[i] >= '0') && (pb[i] <= '9'); i++)
	{
		uValue = uValue * 10 + (UINT) (pb[i] - '0');
	}
	if (!uValue)
	{
		return 0;
	}
	while ((i < uLen) && (pb[i] != '\n'))
	{
		i++;
	}
	if (i < uLen)
	{
		i++;
	}
	*puLineEnd = i;
	return uValue;
}

// Whole-buffer UTF-8 check. Pure ASCII answers FALSE on purpose: a file with no
// non-ASCII in it should take the path it always did.
static BOOL LooksLikeUtf8(const BYTE *pb, UINT uLen)
{
	UINT i = 0;
	BOOL bMultibyte = FALSE;

	while (i < uLen)
	{
		BYTE b = pb[i];
		UINT uTail;
		UINT k;

		if (b < 0x80)
		{
			i++;
			continue;
		}
		if ((b & 0xE0) == 0xC0)
		{
			if (b < 0xC2)      // overlong
			{
				return FALSE;
			}
			uTail = 1;
		}
		else if ((b & 0xF0) == 0xE0)
		{
			uTail = 2;
		}
		else if ((b & 0xF8) == 0xF0)
		{
			if (b > 0xF4)
			{
				return FALSE;
			}
			uTail = 3;
		}
		else
		{
			return FALSE;
		}
		if (i + uTail >= uLen)
		{
			return FALSE;
		}
		for (k = 1; k <= uTail; k++)
		{
			if ((pb[i + k] & 0xC0) != 0x80)
			{
				return FALSE;
			}
		}
		bMultibyte = TRUE;
		i += uTail + 1;
	}
	return bMultibyte;
}

// Script bytes -> a NUL-terminated wide buffer, freeing the source.
static PWSTR DecodeScript(PBYTE pbFile, UINT uSize)
{
	UINT uCodePage = CP_ACP;   // "already UTF-16LE" is handled before this
	UINT uSkip = 0;
	UINT uDirectiveEnd = 0;
	BOOL bSwap = FALSE;
	BOOL bUtf16 = FALSE;
	PWSTR pwzOut;
	UINT uChars;
	INT n;

	if ((uSize >= 2) && (pbFile[0] == 0xFF) && (pbFile[1] == 0xFE))
	{
		bUtf16 = TRUE;
	}
	else if ((uSize >= 2) && (pbFile[0] == 0xFE) && (pbFile[1] == 0xFF))
	{
		bUtf16 = TRUE;
		bSwap = TRUE;
		uSkip = 2;
	}
	else
	{
		uCodePage = ScriptCodeDirective(pbFile, uSize, &uDirectiveEnd);
		if (uCodePage)
		{
			// The line is blanked after decoding, so the numbering holds.
		}
		else if ((uSize >= 3) && (pbFile[0] == 0xEF) && (pbFile[1] == 0xBB) && (pbFile[2] == 0xBF))
		{
			uCodePage = CP_UTF8;
			uSkip = 3;
		}
		else if (LooksLikeUtf8(pbFile, uSize))
		{
			uCodePage = CP_UTF8;
		}
		else
		{
			uCodePage = CP_ACP;
		}
	}

	if (bUtf16)
	{
		UINT uCount = (uSize - uSkip) / 2;
		PBYTE pb = pbFile + uSkip;
		for (UINT k = 0; k < uCount; k++)
		{
			if (bSwap)
			{
				BYTE t = pb[2 * k];
				pb[2 * k] = pb[2 * k + 1];
				pb[2 * k + 1] = t;
			}
		}
		// The buffer is already the wide form of the text.
		return (PWSTR) ((bSwap) ? (pbFile + uSkip) : pbFile);
	}

	uChars = (uSize - uSkip) + MD5Str_Len;
	pwzOut = (PWSTR) UMemAlloc((uChars + 1) * sizeof(WCHAR));
	n = MultiByteToWideChar(uCodePage, 0, (PCSTR) (pbFile + uSkip), (INT) (uSize - uSkip), pwzOut, (INT) uChars);
	if (n < 0)
	{
		n = 0;
	}
	pwzOut[n] = 0;
	UMemFree(pbFile);

	if (uDirectiveEnd)
	{
		UINT k = 0;
		while ((k < uDirectiveEnd) && (pwzOut[k]) && (pwzOut[k] != '\n'))
		{
			pwzOut[k++] = ' ';
		}
	}
	return pwzOut;
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT LoadFile(PTSTR ptzPath)
{	
	// MAX_PATH, not MAX_NAME, and a bounded copy. UStrCopy is an unbounded
	// wcscpy, and MAX_NAME is 80: a script path longer than 79 characters ran
	// off the end of this buffer. The damage did not look like a crash either -
	// it showed up as an unrelated window and a process that never exited.
	TCHAR tzPath[MAX_PATH];
	UStrCopyN(tzPath, ptzPath, _NumOf(tzPath));

	UINT uSize = -1;
	PBYTE pbFile = (PBYTE) UFileLoad(tzPath, NULL, &uSize);
	if (pbFile == NULL)
	{
		return S_FALSE;
	}

	// Set current directory to COMMAND FILE directory
	PTSTR p = UStrRChr(tzPath, '\\');
	if (p)
	{
		p[1] = 0;
		SetCurrentDirectory(tzPath);
		p[0] = 0;		
		tzPath[2] = 0;
		SetEnvironmentVariable(TEXT("CurDrv"), tzPath);
	}

	// A structured script says so on its first line. Its semantics belong to the
	// new front end, so it never reaches the text parser below.
	if (Lang::IsMarker(pbFile, uSize))
	{
		HRESULT hrStructured = Lang::RunBytes(pbFile, uSize, tzPath);

		UMemFree(pbFile);
		return hrStructured;
	}

	// Decode the script, then hand the result on as the file text. The Win32
	// build runs scripts as ANSI, so the wide form is an intermediate there.
	{
		PWSTR pwzScript = DecodeScript(pbFile, uSize);
#ifdef _UNICODE
		p = pwzScript;
		pbFile = (PBYTE) p;
#else
		INT nAnsi = WideCharToMultiByte(CP_ACP, 0, pwzScript, -1, NULL, 0, NULL, NULL);
		p = (PTSTR) UMemAlloc((nAnsi + MD5Str_Len) * sizeof(CHAR));
		WideCharToMultiByte(CP_ACP, 0, pwzScript, -1, p, nAnsi, NULL, NULL);
		if ((PBYTE) pwzScript != pbFile)
		{
			UMemFree(pwzScript);
		}
		pbFile = (PBYTE) p;
#endif
	}

	PTSTR pCurFileStart = p; // start of the file
	pFileStart = pCurFileStart;
	BOOL IsSUB = FALSE;      // inside a sub body

		InsnList script;
	InsnList *prevScript = g_pScript;
	if (ScriptParse((PTSTR) pbFile, &script) != S_OK)
	{
		UMemFree(pbFile);
		return E_OUTOFMEMORY;
	}
	g_pScript = &script;
	{
		// A loaded script is a scope too, so a variable made at its top level does
		// not leak into whatever loads the next file.
		PeVar::ScopeGuard scope;
		ScriptRunTop(&script);
	}
	g_pScript = prevScript;
	ScriptFree(&script);

	UMemFree(pbFile);
	return S_OK;
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// Load command file
// LOAD <file> -- run another script now.
//
// A path that starts with a backslash is looked for under every drive, which is
// what makes "LOAD \Windows\System32\disposition\Post\Link.phos" work whatever
// letter the PE's own volume ended up with. The current file's cursor is saved and
// put back around the load, so the caller carries on from where it was.
HRESULT Load(CoreContext& ctx, PTSTR ptzPath)
{
	PTSTR ptzSavedFileStart = pFileStart;

	if (*ptzPath == '\\')
	{
		TCHAR tzRoots[MAX_NAME];

		ptzPath++;                      // the rest of the path, with no root on it
		GetLogicalDriveStrings(_NumOf(tzRoots), tzRoots);
		for (PTSTR ptzRoot = tzRoots; *ptzRoot; ptzRoot += UStrLen(ptzRoot) + 1)
		{
			TCHAR tzFull[MAX_NAME];

			UStrPrint(tzFull, TEXT("%s%s"), ptzRoot, ptzPath);
			if (UFileExist(tzFull))
			{
				LoadFile(tzFull);
			}
		}
	}
	else if (UFileExist(ptzPath))
	{
		LoadFile(ptzPath);
	}

	pFileStart = ptzSavedFileStart;
	return S_OK;
}
//////////////////////////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
// Init desktop
HRESULT Main(CoreContext& ctx, PTSTR ptzCmd)
{
	if (!FindResource(g_hInst, _MakeIntRes(IDR_MENU1),RT_MENU))
	{
		return S_FALSE;
	}

	// The folder variables - %Desktop%, %Programs%, %StartMenu%, %Startup%,
	// %Favorites%, %SendTo%, %Personal%, %QuickLaunch% - go into the process
	// environment before the script runs, because every child inherits them and
	// a boot chain depends on that.
	//
	// A boot configuration can reach %Desktop% before anything has run INIT, and
	// INI files of that kind are a pile of "LINK %Desktop%\..." lines. Without
	// this setup there is no expanded value to use: the output goes into a
	// directory literally called "%Desktop%", and the command still reports
	// success.
	{
		TCHAR tzEnv[MAX_NAME];
		tzEnv[0] = 0;
		SetEnvironment(tzEnv, FALSE, TRUE);
	}

	// Switch desktop - and record what happened, because every one of these
	// three calls can fail and none of them says so. This runs in the boot
	// driver, before any script has opened a trace channel, so the result goes
	// to the registry instead, where it can be read afterwards.
	{
		TCHAR tzNames[MAX_NAME];
		TCHAR tzProbe[MAX_STR];
		HDESK hDesk = OpenDesktop(TEXT("Default"), 0, TRUE, GENERIC_ALL);
		DWORD dwOpenErr = GetLastError();
		BOOL bSet = FALSE;
		BOOL bSwitch = FALSE;

		if (hDesk)
		{
			bSet = SetThreadDesktop(hDesk);
			if (bSet)
			{
				bSwitch = SwitchDesktop(hDesk);
			}
			CloseDesktop(hDesk);
		}
		DesktopNamesText(tzNames, _NumOf(tzNames));
		UStrPrint(tzProbe, TEXT("open=%u err=%u set=%u switch=%u %s"),
			hDesk ? 1 : 0, dwOpenErr, bSet, bSwitch, tzNames);
		SHSetValue(HKEY_LOCAL_MACHINE, PELOGON, TEXT("DesktopProbe"), REG_SZ,
			tzProbe, UStrLen(tzProbe) * sizeof(TCHAR));
	}
	// Execute LOAD command
	if (ptzCmd[0])
	{
		TCHAR tzCmd[MAX_PATH];
		UStrPrint(tzCmd + GetModuleFileName(NULL, tzCmd, MAX_PATH), TEXT(" LOAD %s"), ptzCmd);
		Exec(g_ctx, tzCmd);		
	}

	MainWindowHwnd = CreateMainWindow(g_hInst/*, SW_HIDE*/); 

	UThreadClose(UThreadCreate((UPROC) MainThread, NULL));

	SetProcessWorkingSetSize(CurrentProcess,-1,-1);

	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	// Exit
	InstallHookKeyBoard(FALSE);
	return (HRESULT) msg.wParam;
}
//////////////////////////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////
HRESULT CommandGroup(PTSTR ptzCurrent)
{
	// A command group only exists when the branch is present: FIND/IFEX with no
	// '!' has no else-branch. Nothing to run is not a failure.
	if (ptzCurrent == NULL)
	{
		return 0;
	}

	HRESULT hResult;
	PTSTR ptzNextCom = UStrChr(ptzCurrent, '|');

	if (ptzNextCom)
	{
		*ptzNextCom++ =0 ;
		Command(ptzCurrent);
		// The group's result is the last command's, not a count of failures.
		// The text form returns 0 for a group whose *middle* command failed
		// ("TEAM FILE a|FILE b(missing)|FILE c|FILE d"), which a sum of
		// failures cannot produce.
		hResult = CommandGroup(ptzNextCom);
	}
	else
	{
		//atoi
		//_wtof
		hResult = Command(ptzCurrent);
	}
	return hResult;
}
/////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////
HRESULT Team(CoreContext& ctx, PTSTR ptzCurrent)
{
	return CommandGroup(ptzCurrent);
}
/////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
// FORX - enumeration as a loop
//
//   FORX [flags] <files>,<variable>,[times],<command and arguments>
//
// times is how many matches to act on, and zero or less means all of them. The
// variable is set to each match, and only then is the command expanded and run:
// that ordering is why a reference to the variable survives the first expansion
// pass. An unset %name% stays literal, so the iteration is what gives it a value.
//
// Only the plain file form is read; /S, /O, /size and the * variable table forms
// are not.
/////////////////////////////////////////////////////////////////////////////
static VOID ForxSetItem(PCTSTR ptzName, PCTSTR ptzValue)
{
	TCHAR tzPair[MAX_STR];

	if (PeVar::IsPeName(ptzName))
	{
		PeVar::Set(ptzName, ptzValue);
		return;
	}
	UStrPrint(tzPair, TEXT("%s=%s"), ptzName, ptzValue);
	SetEnvironment(tzPair, FALSE, FALSE);
}

HRESULT Forx(CoreContext& ctx, PTSTR ptzCmd)
{
	PTSTR ptzSet = ptzCmd;
	PTSTR ptzName = UStrChr(ptzSet, ',');
	PTSTR ptzCount = NULL;
	PTSTR ptzBody = NULL;
	PTSTR ptzBodyRaw = NULL;
	INT nLimit = 0;
	INT nCount = 0;
	WIN32_FIND_DATA FindData;
	HANDLE hFind;

	if (!ptzName)
	{
		return S_FALSE;
	}
	*ptzName++ = 0;

	ptzCount = UStrChr(ptzName, ',');
	if (ptzCount)
	{
		*ptzCount++ = 0;
		ptzBody = UStrChr(ptzCount, ',');
		if (ptzBody)
		{
			*ptzBody++ = 0;
		}
	}

	// Both fields are required. The text form fails when the command field is
	// missing altogether, and reads the count from it when the field is there but
	// empty ("FORX set,var,1,"), which is how a script asks for the match count.
	if (!ptzCount || !ptzBody)
	{
		return S_FALSE;
	}
	nLimit = UStrToInt(UStrTrim(ptzCount));

	// The command comes from the unexpanded copy: it runs once per match and a
	// reference in it has to be expanded after the variable has its value. The
	// three fields in front of it are the expanded ones, which is what a path or a
	// variable name needs.
	{
		PTSTR p = UStrTrim(ctx.RawCommand + LEN_Command);
		UINT uField;

		for (uField = 0; uField < 3; uField++)
		{
			PTSTR q = UStrChr(p, ',');
			if (!q)
			{
				break;
			}
			p = q + 1;
		}
		if (uField == 3)
		{
			ptzBodyRaw = p;
		}
	}

	hFind = FindFirstFile(UStrTrim(ptzSet), &FindData);
	if (hFind != INVALID_HANDLE_VALUE)
	{
		do
		{
			TCHAR tzItem[MAX_PATH];
			PTSTR ptzSlash;

			// Default is files only. The @ and $ prefixes that change that, and
			// the flags in front of the set, are still on the list.
			if (FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			{
				continue;
			}

			// FindFirstFile hands back the name alone, so put the directory back.
			UStrCopy(tzItem, ptzSet);
			ptzSlash = UStrRChr(tzItem, '\\');
			if (ptzSlash)
			{
				ptzSlash[1] = 0;
			}
			else
			{
				tzItem[0] = 0;
			}
			UStrCat(tzItem, FindData.cFileName);

			ForxSetItem(ptzName, tzItem);
			nCount++;
			if (ptzBodyRaw && ptzBodyRaw[0])
			{
				Command(ptzBodyRaw);
			}
		}
		while (((nLimit <= 0) || (nCount < nLimit)) && FindNextFile(hFind, &FindData));
		FindClose(hFind);
	}

	if (!ptzBodyRaw || !ptzBodyRaw[0])
	{
		// With no command the variable comes back as the number of matches.
		TCHAR tzCount[MAX_NAME];
		UStrPrint(tzCount, TEXT("%d"), nCount);
		ForxSetItem(ptzName, tzCount);
	}
	return S_OK;
}
/////////////////////////////////////////////////////////////////////////////
