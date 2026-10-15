// Wait for a key and report which one -- what WAIT with no time is built on.
//
// A key code that is a letter or a digit also lands in PessKey= for a script to
// read. The update is rate-limited: a key is only accepted when a second has gone
// by since the last one, which is what keeps a held key from filling the queue.
HRESULT Pause(UINT KeyCode)
{
	TCHAR tzPressed[MAX_NAME];
	UINT uPressed = 0;

	if (KeyCode)
	{
		if (GetAsyncKeyState(KeyCode) & 0x8000)
		{
			uPressed = KeyCode;
		}
	}
	else
	{
		BOOL bPressed = FALSE;

		while (!bPressed)
		{
			for (UINT i = 8; i < 255; i++)
			{
				if ((GetAsyncKeyState(i) & 0x8000) &&
					((GetTickCount() - timestart_condition) > 1000))
				{
					timestart_condition = GetTickCount();
					uPressed = i;
					bPressed = TRUE;
					break;
				}
			}
		}
	}

	// "PessKey" is the documented spelling, and scripts read it, so it stays.
	UStrCopy(tzPressed, TEXT("PessKey="));
	if (((uPressed > 64) && (uPressed < 91)) || ((uPressed > 47) && (uPressed < 58)))
	{
		TCHAR tzKey[2] = { (TCHAR) uPressed, 0 };

		UStrCat(tzPressed, tzKey);
	}
	SetEnvironment(tzPressed, FALSE, FALSE);
	return uPressed;
}
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
HRESULT IFEx_Or_Find(PTSTR ptzCmd,bool GetFree)
{
	BOOL hResult = false;
	DWORD Var1 = 0;
	DWORD Var2 = 0;

	PTSTR ptzCommand1 = UStrChr(ptzCmd, ',');
	PTSTR ptzCommand2 = NULL;	// no else-branch unless '!' is present

	if (ptzCommand1) 
	{
		*ptzCommand1++ = 0; 

		ptzCommand2 = UStrChr(ptzCommand1, '!');
		if (ptzCommand2)
		{
			*ptzCommand2 = 0;
			ptzCommand2++;
		}

				if (ptzCmd[0] == '$')
		{
			// IFEX and FIND are opposite here, and that has to come from the documentation
			// rather than from the code: IFEX's '$' is a NUMERIC comparison, FIND's '$' is a
			// STRING comparison. Expanding both sides through ExpandEnvironmentStrings
			// would make a plain 'IFEX =1' come out false and its branch never run.
			PTSTR ptzOp = ptzCmd + 1;
			while (*ptzOp && (*ptzOp != '<') && (*ptzOp != '>') && (*ptzOp != '='))
			{
				ptzOp++;
			}

			if (*ptzOp)
			{
				TCHAR tzOp = *ptzOp;
				PTSTR ptzRight = ptzOp + 1;
				if (*ptzRight == '=')
				{
					ptzRight++;		// >= and <= compare the same way as > and <
				}
				*ptzOp = 0;

				PTSTR ptzLeft = UStrTrim(ptzCmd + 1);
				ptzRight = UStrTrim(ptzRight);

				if (GetFree)
				{
					// IFEX: numeric
					INT nLeft = UStrToInt(ptzLeft);
					INT nRight = UStrToInt(ptzRight);
					if (tzOp == '=')      { hResult = (nLeft == nRight); }
					else if (tzOp == '>') { hResult = (nLeft > nRight); }
					else                  { hResult = (nLeft < nRight); }
				}
				else
				{
					// FIND: string
					INT nCmp = UStrCmpI(ptzLeft, ptzRight);
					if (tzOp == '=')      { hResult = (nCmp == 0); }
					else if (tzOp == '>') { hResult = (nCmp > 0); }
					else                  { hResult = (nCmp < 0); }
				}

				*ptzOp = tzOp;		// the instruction text can be executed again
			}
		}
		else
		{
			// Four, not three: "%.3s" writes three characters and the
			// terminator. The array was three wide, so the terminator went one
			// past the end -- harmless while the bounded printer truncates
			// instead, but only because it then wrote two characters where the
			// comparisons below read three, which silently broke them.
			TCHAR KeyWord[4];
			UStrPrint(KeyWord, TEXT("%.3s"), ptzCmd); // the keyword: MEM, KEY or a drive 
			if ((ptzCmd[3] == '>') || (ptzCmd[3] == '<') || (ptzCmd[3] == '=') /*|| (ptzCmd[3] == '!')*/)
			{
				Var2 = UStrToInt(ptzCmd + 4); // the value on the right
				if (!UStrCmpNI(KeyWord, TEXT("MEM"), 3))
				{
					Var1 = (GetFree? GetFreeMemo(): GetTotalMemo());
				}
				else if (!UStrCmpNI(KeyWord, TEXT("KEY"), 3))
				{
					Var1 = Pause(Var2);
				}
				else
				{
					Var1 = (GetFree? GetDiskFree(KeyWord): GetDiskTotal(KeyWord));
				}

				ptzCmd = ptzCmd + 3 ;      // the operator

				if (*ptzCmd == '=')
				{
					hResult = (Var1 == Var2);
				}
				else if (*ptzCmd == '>')
				{
					hResult = (Var1 > Var2);
				}
				else if (*ptzCmd == '<')
				{
					hResult = (Var1 < Var2);			
				}
			}
			else		
			{		
				if (GetFree)
				{
					hResult = UFileExist(ptzCmd);
				}
				else
				{
					hResult = (FindProc(ptzCmd) > 0);
				}
			}
		}
	}
	return (hResult? CommandGroup(ptzCommand1): CommandGroup(ptzCommand2));
}
//////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
HRESULT IFEx(CoreContext& ctx, PTSTR ptzCmd)
{
	return IFEx_Or_Find(ptzCmd,true);
}
//////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
HRESULT Find(CoreContext& ctx, PTSTR ptzCmd)
{
	return IFEx_Or_Find(ptzCmd,false);
}
//////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
// WAIT <ms> waits that long; a WAIT with no number hands over to the key wait,
// whose answer is the key code - so what this reports is whether one was pressed.
HRESULT Wait(CoreContext& ctx, PTSTR ptzCmd)
{
	INT iTime = UStrToInt(ptzCmd);

	if (iTime > 0)
	{
		return TimeDelay((DWORD) iTime);
	}
	return (Pause(NULL) > 0) ? S_OK : S_FALSE;
}
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
// The splash-image capability. Logo() below is the text front end for it
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

		UStrCopy(tzImage, (PTSTR) ptzImage);
		tzDrawTextDone = FALSE;
		ExistLogoImage = FALSE;
		UThreadClose(UThreadCreate((UPROC) LogoCMD, tzImage));
		WaitLogo(FALSE);	// wait for the logo window to come up
		DisplayAppDesc();	// the build line in the corner
	}
	else
	{
		// No image named: take the splash down.
		PostMessage(s_hLogo, WM_COMMAND, IDCANCEL, 0);
		DeleteObject(hfont);
	}
	return S_OK;
}

HRESULT Logo(CoreContext& ctx, PTSTR ptzCmd)
{
	return ShowLogo(ptzCmd);
}
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
// The error a script reads back as %Error%: a failing result means "the Win32
// error from the call that just failed", a success clears it.
VOID SetError(HRESULT eResult)
{
	TCHAR tzError[MAX_NAME];

	LastError = eResult ? GetLastError() : 0;
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
//   * streaming. A buffer that is written only when LOGS is called a second time
//     leaves nothing to look at until the script ends, which is useless when the
//     script is what died. In stream mode every command is flushed as it runs.
//
// Set PHOS_DEBUG_LOG=<target> in the environment -- a file, or a device such
// as \\.\COM1 -- to turn this on at startup. With the variable unset the
// behaviour is exactly as before.
//////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT Logs(CoreContext& ctx, PTSTR ptzCmd);   // defined in commands/fs.cpp

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
// File targets still open and close per write, so what has been written can be
// read while the boot is still running.
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
		Logs(g_ctx, tzTarget);
	}
}

// Append to the trace buffer, growing it when it has to. A file trace is written
// in one go when the channel closes, so everything the chain logs has to fit,
// and the trail of a task is copied into the log of the script that ran it as
// well. A fixed buffer overrun there lands during the final flush, and the
// symptom is a boot that stops dead with an empty trace.
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
	// Report the command's own result. Substituting GetLastError() for it would
	// report whatever the previous call left behind instead: a KILL that found
	// nothing would come out as 18, a missing CALL as 0.
	HRESULT hResult = (eResult == E_NOTIMPL) ? S_FALSE : eResult;

	// One line per executed command, shaped like the text form implementation's
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
	// The result is printed unsigned. The text form does that, and it matters
	// for any HRESULT with the top bit set: E_INVALIDARG comes out as
	// 2147942487, not as a negative number.
	// The elapsed time rides at the end of the line. It is a measurement of the
	// line just run, and it is what makes "would concurrency help here" an
	// answerable question instead of an opinion.
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
