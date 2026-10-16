// What is left behind when this process dies.
//
// In a PE the failure looks like a black screen: the process is gone, the
// screen still shows whatever was drawn last, and nothing has been written
// anywhere. The image runs from a ramdisk, so a file next to the executable
// would not survive the reboot that follows -- which is why this writes to the
// debug channel PHOS_DEBUG_LOG already opened, normally a serial port, and not
// to a file.
//
// Everything here is written for a process that has already lost control:
//
//   * no heap, no CRT formatting, no C++ objects in the handler path -- only
//     preallocated static buffers and raw WriteFile;
//   * numbers are formatted by hand;
//   * the handler records and returns EXCEPTION_EXECUTE_HANDLER. It does not
//     try to carry on: after an access violation the heap may already be
//     damaged, and "continue anyway" is pretending otherwise.
//
// The breadcrumbs are the part that makes the record useful. An address and an
// exception code on their own say "something went wrong in phos.exe"; `line
// 37, verb proc.run` says where to look.

#include <stdlib.h>
#include <signal.h>
#include <exception>

// --- breadcrumbs ------------------------------------------------------------
//
// volatile because the optimiser must not keep these in registers: the point is
// that they are in memory when the machine stops.
static volatile LONG  g_crashLine = 0;
static volatile LONG  g_crashDepth = 0;
static volatile PVOID g_crashVerb = NULL;		// points into the host table: static, never freed
static volatile PVOID g_crashScript = NULL;		// ditto, or NULL

VOID CrashSetLine(int line)          { g_crashLine = (LONG) line; }
VOID CrashSetDepth(int depth)        { g_crashDepth = (LONG) depth; }
VOID CrashSetVerb(PCTSTR verb)       { g_crashVerb = (PVOID) verb; }
VOID CrashSetScript(PCTSTR script)   { g_crashScript = (PVOID) script; }

// --- the writer -------------------------------------------------------------

// One static buffer, appended to by hand. Nothing here allocates.
//
// It is UTF-16, because that is what the trace channel already is: XLogWrite
// hands UFileWrite a TCHAR buffer and its length in bytes. Writing narrow bytes
// here would have left one channel carrying two encodings, which is worse for
// whoever reads the file than either choice on its own.
#define CRASH_BUF_CHARS 2048
static TCHAR  g_crashBuf[CRASH_BUF_CHARS];
static UINT   g_crashLen = 0;

static VOID CrashReset(VOID)
{
	g_crashLen = 0;
	g_crashBuf[0] = 0;
}

static VOID CrashPut(PCSTR ptzText)
{
	while (*ptzText && (g_crashLen + 1 < CRASH_BUF_CHARS))
	{
		g_crashBuf[g_crashLen++] = (TCHAR) *ptzText++;
	}
	g_crashBuf[g_crashLen] = 0;
}

// The verb names and the script path arrive as wide strings -- they come from
// the host table and from the path the caller passed. The first version printed
// them as if they were narrow and produced "script: X", one character followed
// by the wide NUL -- so they are copied as wide text.
static VOID CrashPutWide(PCTSTR ptzText)
{
	if (!ptzText)
	{
		CrashPut("(none)");
		return;
	}
	while (*ptzText && (g_crashLen + 1 < CRASH_BUF_CHARS))
	{
		g_crashBuf[g_crashLen++] = *ptzText;
		ptzText++;
	}
	g_crashBuf[g_crashLen] = 0;
}

static VOID CrashPutHex(UINT_PTR uValue, UINT uDigits)
{
	static const char cDigits[] = "0123456789ABCDEF";

	CrashPut("0x");
	for (UINT i = uDigits; i > 0; i--)
	{
		TCHAR c = cDigits[(uValue >> ((i - 1) * 4)) & 0xF];
		if (g_crashLen + 1 < CRASH_BUF_CHARS)
		{
			g_crashBuf[g_crashLen++] = c;
		}
	}
	g_crashBuf[g_crashLen] = 0;
}

static VOID CrashPutDec(LONG lValue)
{
	char abDigits[16];
	UINT uCount = 0;
	BOOL bNegative = (lValue < 0);
	ULONG uValue = bNegative ? (ULONG) (-(LONG64) lValue) : (ULONG) lValue;

	if (bNegative)
	{
		CrashPut("-");
	}
	do
	{
		abDigits[uCount++] = (char) ('0' + (uValue % 10));
		uValue /= 10;
	}
	while (uValue && (uCount < _NumOf(abDigits)));

	while (uCount)
	{
		char c = abDigits[--uCount];

		if (g_crashLen + 1 < CRASH_BUF_CHARS)
		{
			g_crashBuf[g_crashLen++] = c;
		}
	}
	g_crashBuf[g_crashLen] = 0;
}

// The channel PHOS_DEBUG_LOG opened, if it opened one. A device (COM1) cannot
// be handed to a second writer, so this is the same handle the trace uses.
static VOID CrashEmit(VOID)
{
	if (XLogDevice != INVALID_HANDLE_VALUE)
	{
		DWORD dwWritten = 0;

		WriteFile(XLogDevice, g_crashBuf, g_crashLen * sizeof(TCHAR), &dwWritten, NULL);
		FlushFileBuffers(XLogDevice);
	}
	// Free when a debugger is attached, and harmless when none is.
	OutputDebugString(g_crashBuf);
}

// --- the handlers -----------------------------------------------------------

static LONG WINAPI CrashFilter(PEXCEPTION_POINTERS pInfo)
{
	// No locals that need construction, no calls that allocate: this runs with
	// the process already in an unknown state.
	CrashReset();
	CrashPut("\r\n=== phos crash ===\r\nexception ");
	CrashPutHex((UINT_PTR) pInfo->ExceptionRecord->ExceptionCode, 8);
	CrashPut("  address ");
	CrashPutHex((UINT_PTR) pInfo->ExceptionRecord->ExceptionAddress, sizeof(UINT_PTR) * 2);
	CrashPut("\r\nbreadcrumb: line ");
	CrashPutDec(g_crashLine);
	CrashPut("  depth ");
	CrashPutDec(g_crashDepth);
	CrashPut("\r\nverb: ");
	CrashPutWide((PCTSTR) g_crashVerb);
	CrashPut("\r\nscript: ");
	if (g_crashScript)
	{
		CrashPutWide((PCTSTR) g_crashScript);
	}
	else
	{
		CrashPut("(unknown)");
	}
	CrashPut("\r\n=== end ===\r\n");
	CrashEmit();

	return EXCEPTION_EXECUTE_HANDLER;
}

static VOID CrashTerminate(VOID)
{
	CrashReset();
	CrashPut("\r\n=== phos crash ===\r\nterminate()\r\nbreadcrumb: line ");
	CrashPutDec(g_crashLine);
	CrashPut("  depth ");
	CrashPutDec(g_crashDepth);
	CrashPut("\r\n=== end ===\r\n");
	CrashEmit();
	TerminateProcess(GetCurrentProcess(), (UINT) -1);
}

static VOID CrashInvalidParameter(PCTSTR ptzExpression, PCTSTR ptzFunction, PCTSTR ptzFile,
	UINT uLine, UINT_PTR uReserved)
{
	CrashReset();
	CrashPut("\r\n=== phos crash ===\r\ninvalid parameter\r\nbreadcrumb: line ");
	CrashPutDec(g_crashLine);
	CrashPut("\r\n=== end ===\r\n");
	CrashEmit();
	TerminateProcess(GetCurrentProcess(), (UINT) -1);
}

static VOID CrashPureCall(VOID)
{
	CrashReset();
	CrashPut("\r\n=== phos crash ===\r\npure virtual call\r\nbreadcrumb: line ");
	CrashPutDec(g_crashLine);
	CrashPut("\r\n=== end ===\r\n");
	CrashEmit();
	TerminateProcess(GetCurrentProcess(), (UINT) -1);
}

// abort() goes through signal(SIGABRT) on classic CRT and through __fastfail on
// newer ones; __fastfail bypasses the unhandled exception filter entirely, so
// the signal handler is the one that catches it.
static VOID CrashAbort(int)
{
	CrashReset();
	CrashPut("\r\n=== phos crash ===\r\nabort()\r\nbreadcrumb: line ");
	CrashPutDec(g_crashLine);
	CrashPut("\r\n=== end ===\r\n");
	CrashEmit();
	TerminateProcess(GetCurrentProcess(), (UINT) -1);
}

// --- installation -----------------------------------------------------------

VOID CrashInstall(VOID)
{
	// A WerFault dialog on a booting PE is a hang, not a diagnosis.
	SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);

	SetUnhandledExceptionFilter(CrashFilter);
	std::set_terminate(CrashTerminate);
	_set_invalid_parameter_handler(CrashInvalidParameter);
	_set_purecall_handler(CrashPureCall);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	signal(SIGABRT, CrashAbort);
}

#ifdef PHOS_TESTHOOKS
// Only in a test build. The release binary must not contain these strings.
VOID CrashTestTrigger(PCTSTR ptzKind)
{
	if (!UStrCmp(ptzKind, TEXT("av")))
	{
		volatile int *p = (int *) (UINT_PTR) 1;
		*p = 7;
	}
	else if (!UStrCmp(ptzKind, TEXT("div")))
	{
		volatile int iZero = 0;
		volatile int iOne = 1;
		volatile int iResult = iOne / iZero;
		(void) iResult;
	}
	else if (!UStrCmp(ptzKind, TEXT("abort")))
	{
		abort();
	}
}
#endif
