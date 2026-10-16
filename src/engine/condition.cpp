// Wait for a key and report which one -- what WAIT with no time is built on.
//
// A key code that is a letter or a digit also lands in PessKey= for a script to
// read. The update is rate-limited: a key is only accepted when a second has gone
// by since the last one, which is what keeps a held key from filling the queue.
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
// The splash-image capability. Logo() below is the legacy text front end for it
// -- and for boot.clear(), which is the same operation with no image named.
HRESULT ShowLogo(PCTSTR ptzImage)
{
	if ((ptzImage != NULL) && (ptzImage[0] != 0))
	{
		// LogoCMD blanks the path it is handed once the image is loaded, and it
		// runs on its own thread, so it gets a buffer that is its own -- not a
		// pointer into the script's string value, which is what a caller in the
		// language passes in.
		static TCHAR tzImage[MAX_PATH];

		if (!UStrCopyTo(tzImage, ptzImage))
		{
			return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
		}
		tzDrawTextDone = FALSE;
		ExistLogoImage = FALSE;
		UThreadClose(UThreadCreate((UPROC) LogoCMD, tzImage));
		WaitLogo();	// wait for the logo window to come up
		DisplayAppDesc();	// put this build's own description on it
	}
	else
	{
		// No image named: take the splash down.
		PostMessage(s_hLogo, WM_COMMAND, IDCANCEL, 0);
		DeleteObject(hfont);
	}
	return S_OK;
}

/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
// The error a script reads back as %Error%: a failing result means "the Win32
// error from the call that just failed", a success clears it.
VOID SetError(HRESULT eResult)
{
	TCHAR tzError[MAX_NAME];

	LastError = (eResult != S_OK) ? GetLastError() : 0;
	UStrPrint(tzError, TEXT("%d"), LastError);
	SetEnvironmentVariable(TEXT("Error"), tzError);
}
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////
// Debug log channel
//
// LOGS records an execution trace: every command and its result. Two things
// were missing for using it as a debug channel during PE startup, when there
// is no console to look at:
//
//   * a device target. UFileOpen opens with CREATE_ALWAYS, which a COM port
//     rejects, so device paths are opened with OPEN_EXISTING instead.
//   * streaming. The buffer used to be written only when LOGS was called a
//     second time, so nothing appeared until the script ended. In stream mode
//     every command is flushed as it runs.
//
// Set PHOS_DEBUG_LOG=<target> in the environment -- a file, or a device such
// as \\.\COM1 -- to turn this on at startup. With the variable unset the
// behaviour is exactly as before.
//////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT LogOpen(PTSTR ptzTarget, UINT uBlockSize);
VOID LogClose(VOID);

static HANDLE XLogOpen(PTSTR ptzTarget, BOOL bStream)
{
	if ((ptzTarget[0] == '\\') && (ptzTarget[1] == '\\'))
	{
		// Device path such as \\.\COM1: OPEN_ALWAYS is not valid for devices.
		return CreateFile(ptzTarget, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_EXISTING, 0, NULL);
	}
	return UFileOpen(ptzTarget, bStream ? UFILE_APPEND : UFILE_WRITE);
}

// A device target keeps its handle for the life of the log. Opening and closing
// a serial port once per trace line makes the host end of the line see a new
// "session" every time, and VMware's serial-to-file capture drops the head of
// the output when that happens - a long boot chain then arrives with its first
// half missing, which is the worst possible failure mode for a debug channel.
// File targets still open and close per write, because the harness reads them
// while the boot is still running.
static HANDLE XLogDevice = INVALID_HANDLE_VALUE;

// When the channel was opened, for the per-line elapsed time. Zero until then.
static DWORD g_uXLogStart = 0;

static VOID XLogWrite(BOOL bStream)
{
	if (!LogFileData)
	{
		return;
	}

	BOOL bDevice = (LogFileName[0] == '\\') && (LogFileName[1] == '\\');
	HANDLE hFile;

	if (bDevice)
	{
		if (XLogDevice == INVALID_HANDLE_VALUE)
		{
			XLogDevice = XLogOpen(LogFileName, bStream);
		}
		hFile = XLogDevice;
	}
	else
	{
		hFile = XLogOpen(LogFileName, bStream);
	}

	if (hFile != INVALID_HANDLE_VALUE)
	{
		UFileWrite(hFile, LogFileData, UStrLen(LogFileData) * sizeof(TCHAR));
		if (!bDevice)
		{
			UFileClose(hFile);
		}
	}

	if (bStream)
	{
		if (bDevice)
		{
			// A device stream keeps the byte-order mark, so the host end can
			// start decoding from any line. Drop the text just written.
			LogFileData[0] = 0xFEFF;
			LogFileData[1] = 0;
		}
		else
		{
			// A file gets one mark, at the head. Resetting to the mark here
			// would put a stray U+FEFF in front of every line.
			LogFileData[0] = 0;
		}
	}
}

static VOID XLogCloseDevice(VOID)
{
	if (XLogDevice != INVALID_HANDLE_VALUE)
	{
		UFileClose(XLogDevice);
		XLogDevice = INVALID_HANDLE_VALUE;
	}
}

VOID XLogStartFromEnv(VOID)
{
	TCHAR tzTarget[MAX_PATH];
	if (GetEnvironmentVariable(TEXT("PHOS_DEBUG_LOG"), tzTarget, MAX_PATH) > 0)
	{
		g_bXLogStream = TRUE;
		LogOpen(tzTarget, 16);
	}
}

// Append to the trace buffer, growing it when it has to. The buffer used to be
// a fixed MAX_STR * LogSize block that this file appended to with an unbounded
// strcat. A file trace is written in one go when the channel closes, so
// everything the chain logs has to fit in that block -- and once a task's own
// trail started being copied back into the parent's log, it did not. The
// overflow landed during the final flush, which is why the symptom was a boot
// that stopped dead with an empty serial log and no harness output at all.
VOID LogAppend(PCTSTR ptzText)
{
	UINT uHave = LogFileData ? UStrLen(LogFileData) : 0;
	UINT uNeed = uHave + UStrLen(ptzText) + 1;

	if (!LogFileData || (uNeed > LogFileCap))
	{
		UINT uCap = (LogFileCap > 0) ? LogFileCap : (UINT) MAX_STR;

		while (uCap < uNeed)
		{
			uCap *= 2;
		}
		{
			PTSTR ptzNew = (PTSTR) UMemRealloc(LogFileData, sizeof(TCHAR) * uCap);

			if (!ptzNew)
			{
				return;		// keep the trace that is already there
			}
			LogFileData = ptzNew;
		}
		if (LogFileCap == 0)
		{
			LogFileData[0] = 0;
		}
		LogFileCap = uCap;
	}
	UStrCat(LogFileData, (PTSTR) ptzText);
}

VOID SetLogs(PTSTR tzCmdStr,HRESULT eResult)
{	
	// Report the command's own result. The old code replaced any non-zero result
	// with GetLastError(), so the trace showed whatever the previous call left
	// behind: a KILL that found nothing came out as 18, a missing CALL as 0.
	HRESULT hResult = (eResult == E_NOTIMPL) ? S_FALSE : eResult;

	// One line per executed command, shaped like the reference implementation's
	// trace so the two can be compared field by field:
	//   <line>\t[<command>]\t<<result>>\t:<message>
	// The message text is localised and is deliberately not compared.
	TCHAR tzMsg[MAX_PATH];
	tzMsg[0] = 0;
	DWORD uLen = FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, hResult, 0, tzMsg, _NumOf(tzMsg) - 1, NULL);
	if (uLen == 0)
	{
		PTSTR p = tzMsg + LoadString(g_hInst, IDS_CmdErr, tzMsg, MAX_NAME);
		UStrPrintN(p, (size_t) (_NumOf(tzMsg) - (size_t) (p - tzMsg)), TEXT("%08X"), hResult);
	}
	else
	{
		// FormatMessage ends the text with CRLF; a trace line is one line.
		while ((uLen > 0) &&
			((tzMsg[uLen - 1] == '\r') || (tzMsg[uLen - 1] == '\n') || (tzMsg[uLen - 1] == ' ')))
		{
			tzMsg[--uLen] = 0;
		}
	}

	TCHAR tzLine[MAX_STR];
	// The result is printed unsigned. The reference does that, and it matters
	// for any HRESULT with the top bit set: E_INVALIDARG comes out as
	// 2147942487 there, not as a negative number (case 005 shows both forms
	// side by side).
	// The elapsed time rides at the end of the message field on purpose: that
	// field is localised and the differential differ ignores it, so a timing
	// column cannot turn a green comparison red. It is what makes "would
	// concurrency help here" an answerable question instead of an opinion.
	UStrPrint(tzLine, TEXT("%u\t[%s]\t<%u>\t:%s [t=%u]\n"),
		g_ctx.uLineNo, tzCmdStr, (UINT) hResult, tzMsg,
		(UINT) (GetTickCount() - g_uXLogStart));

	LogAppend(tzLine);

	if (g_bXLogStream)
	{
		XLogWrite(TRUE);
	}
}
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////
// The trace channel, as two calls rather than the one text command that spelled
// both directions: "LOGS <target>" opened it, an empty line closed it, and a
// switch to a new target was recorded in the log being closed.
//
// The two halves are what the callers actually need. The boot path opens a
// channel from an environment variable before any script has run; log.open and
// log.close are verbs. Nothing here parses text.
VOID LogClose(VOID)
{
	if (!g_hXLog)
	{
		return;
	}

	// In stream mode each line is already on disk; only the byte-order mark is
	// left in the buffer, and appending that again would leave a stray one
	// behind.
	if (!g_bXLogStream || (LogFileData && (LogFileData[1] != 0)))
	{
		XLogWrite(g_bXLogStream);
	}
	XLogCloseDevice();
	UMemFree(LogFileData);
	LogFileData = NULL;
	LogFileCap = 0;
	g_hXLog = NULL;
}

// uBlockSize is the starting size of the buffer in MAX_STR characters. It sets
// where the buffer starts, not where it ends: LogAppend grows it, so a trace
// longer than the block asked for is kept rather than running off the end of it.
HRESULT LogOpen(PTSTR ptzTarget, UINT uBlockSize)
{
	if (g_hXLog)
	{
		TCHAR tzStr[MAX_STR];

		// The line belongs to the log being closed, so it is written first.
		UStrPrint(tzStr, TEXT("LOGS %s"), ptzTarget);
		SetLogs(tzStr, S_OK);
		LogClose();
	}

	if (!uBlockSize)
	{
		uBlockSize = 16;
	}
	if (!UStrCopyTo(LogFileName, ptzTarget))
	{
		return E_INVALIDARG;
	}

	// UMemRealloc is plain realloc, so this memory is uninitialised. Upstream
	// called UStrLen on it and then wrote only the first half of the BOM, which
	// left the string unterminated: the next line appended past the end of it.
	// Zero the buffer, then start it with a full BOM.
	LogFileCap = (UINT) MAX_STR * uBlockSize;
	LogFileData = (PTSTR) UMemRealloc(LogFileData, sizeof(TCHAR) * LogFileCap);
	if (LogFileData == NULL)
	{
		LogFileCap = 0;
		g_hXLog = NULL;
		return S_FALSE;
	}
	UMemSet(LogFileData, 0, sizeof(TCHAR) * LogFileCap);
	LogFileData[0] = 0xFEFF;
	g_hXLog = (HANDLE) TRUE;
	g_uXLogStart = GetTickCount();		// zero point for the per-line [t=]

	// Every channel streams. It used to be devices only, on the theory that a
	// file is written once when the channel closes -- which is true, and is
	// exactly the problem: a script that fails never reaches the closing call,
	// so the buffer is lost and a boot that died half way leaves a trace
	// indistinguishable from one that finished. The content is the same either
	// way, because a file is opened for append and closed per line.
	g_bXLogStream = TRUE;

	return S_OK;
}
/////////////////////////////////////////////////////////////////////////////
