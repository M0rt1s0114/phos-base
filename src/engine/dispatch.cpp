// Command table: name, handler.
//
// The name has to match in full and be followed by a separator: "ENVIXB X=1" is
// an unknown command, not ENVI with the argument "XB X=1". Longest names must
// come first in this table.
//
// Nothing caps a name at four characters, so text-command dialect names such as
// _ENDFILE, IMPORT and THREAD can be added.
// Characters that may follow a command name: the argument introducers the
// documentation uses, or the end of the line.
static BOOL IsCommandSeparator(TCHAR c)
{
	return (c == 0) || (c == ' ') || (c == '\t') || (c == '^') || (c == '@') ||
		(c == '-') || (c == '+') || (c == '*') || (c == '?') || (c == '<') ||
		(c == '>') || (c == '!') || (c == '=') || (c == ':') || (c == '#') ||
		(c == '&') || (c == '.') || (c == '$') || (c == '|') || (c == ',');
}

struct CommandEntry
{
	PCTSTR name;
	HRESULT (*handler)(CoreContext&, PTSTR);
};

static const CommandEntry c_commands[] =
{
		{ TEXT("MAIN"), Main },
		{ TEXT("LOAD"), Load },
		{ TEXT("EXEC"), Exec },
		{ TEXT("CALL"), Call },
		{ TEXT("REGI"), Regi },
		{ TEXT("ENVI"), Envi },
	{ TEXT("SET"), SetVar },
		{ TEXT("FILE"), File },
		{ TEXT("LINK"), Link },
		{ TEXT("SEND"), Send },
		{ TEXT("WAIT"), Wait },
		{ TEXT("KILL"), Kill },
		{ TEXT("SHUT"), Shut },
		{ TEXT("SHOW"), Show },
		{ TEXT("SERV"), Serv },
		{ TEXT("PAGE"), Page },
		{ TEXT("DISP"), Disp },
		{ TEXT("DISK"), Disk },
		{ TEXT("LOGO"), Logo },
		{ TEXT("TEXT"), Text },
		{ TEXT("LOGS"), Logs },
		{ TEXT("IFEX"), IFEx },
		{ TEXT("FBWF"), Fbwf },
		{ TEXT("WALL"), Wall },
		{ TEXT("RAMD"), Ramd },
		{ TEXT("RUNS"), Runs },
		{ TEXT("NUMK"), Numk },
		{ TEXT("USER"), User },
		{ TEXT("SHEL"), Shel },
	{ TEXT("HIDE"), Hide },
		{ TEXT("FONT"), Font },
		{ TEXT("TEAM"), Team },
		{ TEXT("FIND"), Find },
	{ TEXT("FORX"), Forx },
		{ TEXT("HOTK"), Hotk },
		{ TEXT("INIT"), Init },
		{ TEXT("PATH"), Path },
		{ TEXT("MESS"), Mess },
		{ TEXT("BROW"), Brow },
		{ TEXT("HELP"), Help },
		{ TEXT("EJEC"), Ejec },
		{ TEXT("TEMP"), Temp },
		{ TEXT("MD5C"), Md5c },
		{ TEXT("MOUN"), Moun },
		{ TEXT("RECY"), Recy },
		{ TEXT("FDIR"), Fdir },
		{ TEXT("FEXT"), Fext },
		{ TEXT("FNAM"), Fnam },
		{ TEXT("NAME"), Name },
		{ TEXT("LSTR"), Lstr },
		{ TEXT("RSTR"), Rstr },
		{ TEXT("STRL"), Strl },
		{ TEXT("LPOS"), Lpos },
		{ TEXT("MSTR"), Mstr },
		{ TEXT("SED"), Sed },
		{ TEXT("SIZE"), Size },
		{ TEXT("MDIR"), Mkdir },
		{ TEXT("READ"), Read },
		{ TEXT("WRIT"), Writ },
		{ TEXT("FLNK"), Flnk },
		{ TEXT("BASE"), Base },
		{ TEXT("HASH"), Hash },
		{ TEXT("DATE"), Date },
		{ TEXT("DIR"), Dir },
		{ TEXT("SITE"), Site },
	{ NULL, NULL }
};

// Is this line a dialect command rather than a program command line?
//
// The hotkey and tray menu handlers carry text that can be either, and only the
// command table can tell them apart. The test is the one the dispatcher below
// uses: the name in full, followed by a separator.
BOOL IsCommandLine(PTSTR ptzCmd)
{
	ptzCmd = UStrTrim(ptzCmd);
	if (ptzCmd[0] == 0)
	{
		return FALSE;
	}
	for (UINT uCmd = 0; c_commands[uCmd].name; uCmd++)
	{
		UINT uName = UStrLen(c_commands[uCmd].name);

		if (!UStrCmpNI(ptzCmd, c_commands[uCmd].name, uName) && IsCommandSeparator(ptzCmd[uName]))
		{
			return TRUE;
		}
	}
	return FALSE;
}

HRESULT Command(PTSTR ptzCmd)
{
	HRESULT hResult = E_NOTIMPL;
	// A handler may run nested commands (FIND/IFEX/TEAM) that advance the current
	// line; the trace belongs to this command, so remember where it started.
	UINT uEntryLine = g_ctx.uLineNo;
	ptzCmd = UStrTrim(ptzCmd);

	// Keep the unexpanded text. Almost everything wants the expanded form, but a
	// loop command has to run its tail once per item, and by then there is a value
	// where there was a reference: FORX reads its command from here.
	//
	// Nested calls (FORX running its body, TEAM running a branch, FIND/IFEX
	// running one) pass a pointer that is itself inside this same buffer. The
	// write above moves that text to the front, so the caller's pointer now
	// addresses a stale tail of it - which is how a FORX body came out as a
	// command literally named "CMD=". Take one copy of the caller's text and
	// use that from here on; the pointer itself is not valid any more.
	TCHAR tzRaw[MAX_STR];

	// Everything below is a MAX_STR buffer, and the text arrives from the
	// command line, from a hotkey, or from a script line: refuse what cannot
	// fit instead of running over the end of the stack frame. (A file line that
	// is too long is caught where it is read, by GetNewLine.)
	if (UStrLen(ptzCmd) >= MAX_STR)
	{
		SetLastError(ERROR_BUFFER_OVERFLOW);
		return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
	}
	UStrCopy(tzRaw, ptzCmd);
	UStrCopy(g_ctx.RawCommand, tzRaw);

	// PE variables first: %&name% and its explicit forms are ours, and the Win32
	// call below would only leave them alone anyway.
	TCHAR tzPe[MAX_STR];
	PeVar::Expand(tzPe, MAX_STR, tzRaw);

	TCHAR tzCmdStr[MAX_STR];
	TCHAR tzCmd[MAX_STR] = {0};
	{
		// The answer is the size the expansion needs, and the buffer is not
		// written at all when it does not fit -- so an over-long expansion used
		// to be read back as whatever the stack held.
		DWORD dwExpand = ExpandEnvironmentStrings(tzPe, tzCmd, MAX_STR);

		if ((dwExpand == 0) || (dwExpand > MAX_STR))
		{
			SetLastError(ERROR_BUFFER_OVERFLOW);
			return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
		}
	}
	UStrCopy(tzCmdStr,tzCmd);

	UINT uCmd = 0;
	ptzCmd = tzCmd;
	while (c_commands[uCmd].name)
	{
		UINT uName = UStrLen(c_commands[uCmd].name);
		if (!UStrCmpNI(ptzCmd, c_commands[uCmd].name, uName) && IsCommandSeparator(ptzCmd[uName]))
		{
			ptzCmd += uName;
			ptzCmd =UStrTrim(ptzCmd);
			break;
		}
		uCmd++;
	}

	// Trim quote mark
	while (*ptzCmd == '"')
	{
		ptzCmd++;
		PTSTR p = ptzCmd + UStrLen(ptzCmd) - 1;
		if (*p == '"')
		{
			*p = 0;
		}
		else
		{
			break;
		}
	}

	if (c_commands[uCmd].name)
	{
		hResult = c_commands[uCmd].handler(g_ctx, ptzCmd);
	}
	else
	{
		// Nothing matched. Report 'not found' rather than a bare failure: that is
		// what the code means, and it is the result the text form reports here.
		hResult = (HRESULT) ERROR_FILE_NOT_FOUND;
		SetLastError(ERROR_FILE_NOT_FOUND);
	}
	if (g_hXLog)
	{
		g_ctx.uLineNo = uEntryLine;
		SetLogs(tzCmdStr,hResult);
	}
	if ((hResult > 0) || (LastError > 0))
	{
		SetError(hResult);
	}
	return hResult;
}
//////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////
// Entry
