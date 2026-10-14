HRESULT Logs(CoreContext& ctx, PTSTR ptzCmd)
{
	if (g_hXLog)
	{		
		// The text form implementation flushes and closes a log without recording
		// that closing LOGS itself. A switch to a new target is still recorded, since
		// that line belongs to the log being closed.
		if (ptzCmd[0])
		{
			TCHAR tzStr[MAX_STR];
			UStrPrint(tzStr, TEXT("LOGS %s"),ptzCmd);
			SetLogs(tzStr,S_OK);
		}

		// In stream mode each line is already on disk; only the byte-order mark
		// is left in the buffer, and appending that again would leave a stray
		// one behind.
		if (!g_bXLogStream || (LogFileData && (LogFileData[1] != 0)))
		{
			XLogWrite(g_bXLogStream);
		}
		XLogCloseDevice();
		UMemFree(LogFileData);
		LogFileData = NULL;
		LogFileCap = 0;
	}

	if (ptzCmd[0])
	{

#ifdef _UNICODE

		UINT LogSize = MD5Str_Len;
		PTSTR p = UStrChr(ptzCmd, ',');
		if (p)
		{
			*p++ = 0;
			LogSize = UStrToInt(p);
		}		
		UStrCopy(LogFileName,ptzCmd);
		// UMemRealloc is plain realloc, so this memory is uninitialised. Writing
		// only the first half of the BOM leaves the string unterminated, and the
		// next command's SetLogs then appends past the end of it. Zero the buffer,
		// then start it with a full BOM.
		// LogSize sets the starting size only: LogAppend grows the buffer from
		// here, so a trace longer than the requested block is kept rather than
		// running off the end of it.
		LogFileCap = (UINT) MAX_STR * LogSize;
		LogFileData=(PTSTR)UMemRealloc(LogFileData,sizeof(TCHAR)* LogFileCap);
		if (LogFileData == NULL)
		{
			LogFileCap = 0;
			g_hXLog = NULL;
			return S_FALSE;
		}
		UMemSet(LogFileData,0,sizeof(TCHAR)* LogFileCap);
		LogFileData[0]=0xFEFF;
		g_hXLog = (HANDLE) TRUE;
		g_uXLogStart = GetTickCount();		// zero point for the per-line [t=]
		// Every channel streams. A file that is written only when the channel
		// closes is exactly the problem this avoids: a script that fails never
		// reaches the closing LOGS, so the buffer is lost and a boot that died
		// half way leaves a trace indistinguishable from one that finished. The
		// content is the same either way, because a file is opened for append
		// and closed per line.
		g_bXLogStream = TRUE;

#endif

	}
	else
	{
		g_hXLog = NULL;
	}
	return S_OK;
}
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
HRESULT Mdir(PTSTR ptzCmd)
{
	// Text front end. It answers S_FALSE rather than the Win32 code on
	// failure, and the trace records that value, so the mapping stays put.
	return (DirCreate(ptzCmd) == S_OK) ? S_OK : S_FALSE;
}
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
// PATH -- the working directory, plus the two commands that hang off it.
//
//   PATH <dir>     create the directory and go into it
//   PATH @<dir>    go into it, reporting success even when it cannot
//   PATH #<file>   a file command
//   PATH           go back to where this started
//
// CurrentDir is that starting directory, and it is remembered the first time this
// runs; %SystemRoot%\SYSTEM32 is the stand-in when the shell never gave one.
HRESULT Path(CoreContext& ctx, PTSTR ptzCmd)
{
	if (!CurrentDir[0])
	{
		GetCurrentDirectory(_NumOf(CurrentDir), CurrentDir);
	}

	if (!ptzCmd[0])
	{
		if (!CurrentDir[0])
		{
			ExpandEnvironmentStrings(TEXT("%SystemRoot%\\SYSTEM32"), CurrentDir, _NumOf(CurrentDir));
		}
		return SetCurrentDirectory(CurrentDir) ? S_OK : S_FALSE;
	}

	if (*ptzCmd == '@')
	{
		// A directory that cannot be entered is not an error here: the text form
		// reports success, and failing a boot script over it is worse.
		SetCurrentDirectory(ptzCmd + 1);
		return S_OK;
	}
	if (*ptzCmd == '#')
	{
		return File(g_ctx, ptzCmd + 1);
	}
	return Mdir(ptzCmd);
}
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
