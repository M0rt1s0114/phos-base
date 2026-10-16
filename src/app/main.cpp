INT APIENTRY _tWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PTSTR ptzCmdLine, INT iShowCmd)
{
	INT iResult = S_OK;	
	g_hInst = hInstance;

	// Before anything else that can fail. From here on a crash leaves a record
	// on the debug channel instead of a black screen and nothing else.
	CrashInstall();

	XLogStartFromEnv();
	CurrentProcess = GetCurrentProcess();

#ifdef PHOS_TESTHOOKS
	// Test builds only, and the release binary must not contain this string:
	// the VM rig asks for a specific crash to prove the recorder works.
	if (!UStrCmpNI(ptzCmdLine, TEXT("--crash-test:"), 13))
	{
		CrashTestTrigger(ptzCmdLine + 13);
	}
#endif

	// `--dry-run` is taken off the front first, so it reads the same in front of
	// the boot entry and in front of the phos sub-commands:
	//
	//   phos.exe --dry-run --init
	//   phos.exe --dry-run run X:\boot\start.phos
	//
	// What it does is refuse every verb whose declared side effect is not "read",
	// and trace the refusal. It exists
	// because "what would this script do" is otherwise answered by running it,
	// and in a PE that answer is permanent.
	if (!UStrCmpNI(ptzCmdLine, TEXT("--dry-run"), 9) &&
		((ptzCmdLine[9] == 0) || (ptzCmdLine[9] == ' ') || (ptzCmdLine[9] == '\t')))
	{
		g_bDryRun = TRUE;
		ptzCmdLine += 9;
		while ((*ptzCmdLine == ' ') || (*ptzCmdLine == '\t'))
		{
			ptzCmdLine++;
		}
	}

	// `phos --init` is the boot entry: it resolves the script itself, so the
	// registry CmdLine can be a constant with no path and no %Variable% in it.
	// Main is what switches the desktop, brings up the tray and the shell watcher
	// and then pumps messages, so the boot entry ends there rather than returning.
	{
		Lang::String initScript;
		BOOL InitCli(PTSTR ptzCmdLine, Lang::String& outScript);

		if (ptzCmdLine[0] && InitCli(ptzCmdLine, initScript))
		{
			if (initScript.empty())
			{
				// The script was named (or defaulted to) and is not there. The
				// init helper has already recorded which path; stop here rather
				// than hand Main a file it cannot load, because Main ignores that
				// and then sits in the tray -- a black screen that says nothing.
				return 1;
			}
			NotChineseLangID = (GetSystemDefaultLangID() - 0x0804);
			g_iShowCmd = iShowCmd;
			UStrCopyTo(LogoDescText, UDescStr);
			SetError(NULL);
			iResult = (INT) Main(g_ctx, (PTSTR) initScript.c_str());
			InstallHookKeyBoard(FALSE);
			return iResult;
		}
	}

	// `phos check` and `phos run` are handled here, before anything else looks
	// at the line. The helpers are defined at the end of this file; the
	// declaration is repeated because this body comes first.
	{
		INT iCli = 0;
		BOOL IsCli(PTSTR ptzCmdLine, INT *piResult);

		if (ptzCmdLine[0] && IsCli(ptzCmdLine, &iCli))
		{
			return iCli;
		}
	}

	if (ptzCmdLine[0])
	{
		// Nothing left can be run. Until this round the rest of the line went to
		// the text dispatcher, which is where "LOAD x.ini", "SHUT R" and the
		// rest of the four-letter dialect lived. That is gone, and a line that
		// would have meant something to it has to fail where someone can see
		// why: silently starting nothing is how a boot with a stale registry
		// entry turns into a black screen with no explanation.
		INT RefuseCommandLine(PTSTR ptzCmdLine);

		iResult = RefuseCommandLine(ptzCmdLine);
	}
	else
	{	
		iResult = (INT) ShowAbout();
	}	

	return iResult;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// The `phos` command line, used by the host side check driver.
using Lang::String;
//
// The build is a GUI subsystem program, so console output is not something to
// rely on: check writes its verdict to a file and returns an exit code, which a
// .cmd driver reads. `check` is the one verification that runs on an ordinary
// Windows host. `run` is exposed because the language has a run verb, but the
// rule still stands that real command work is verified in the PE.
//////////////////////////////////////////////////////////////////////////////////////////////////
static VOID SplitCliArgs(PCTSTR ptzLine, std::vector<String>& out)
{
	while (*ptzLine)
	{
		while ((*ptzLine == ' ') || (*ptzLine == '\t'))
		{
			ptzLine++;
		}
		if (!*ptzLine)
		{
			break;
		}
		{
			String s;

			if (*ptzLine == '"')
			{
				ptzLine++;
				while (*ptzLine && (*ptzLine != '"'))
				{
					s += *ptzLine++;
				}
				if (*ptzLine == '"')
				{
					ptzLine++;
				}
			}
			else
			{
				while (*ptzLine && (*ptzLine != ' ') && (*ptzLine != '\t'))
				{
					s += *ptzLine++;
				}
			}
			out.push_back(s);
		}
	}
}

static BOOL WriteCliFile(PCTSTR ptzPath, const String& text)
{
	HANDLE h = CreateFile(ptzPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	if (h == INVALID_HANDLE_VALUE)
	{
		return FALSE;
	}
	{
		int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int) text.size(), NULL, 0, NULL, NULL);

		if (n > 0)
		{
			std::vector<char> buf((size_t) n + 1, 0);
			DWORD written = 0;

			WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int) text.size(), buf.data(), n, NULL, NULL);
			WriteFile(h, buf.data(), (DWORD) n, &written, NULL);
		}
	}
	CloseHandle(h);
	return TRUE;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// The directory this executable is in, or an empty string when it cannot be
// named. It is where init.phos is looked for, and where a startup failure is
// written down.
static String ProgramDirectory(VOID)
{
	TCHAR tzExe[MAX_PATH];
	DWORD uLen = GetModuleFileName(NULL, tzExe, _NumOf(tzExe));
	String s = ((uLen > 0) && (uLen < _NumOf(tzExe))) ? String(tzExe) : String();
	size_t slash = s.find_last_of(L"\\/");

	return (slash != String::npos) ? s.substr(0, slash) : String();
}

// A file the CLI leaves behind goes beside the program, not into "whatever the
// current directory happens to be".
//
// This is a GUI-subsystem binary: there is no console to print a version or a
// usage line to, so both are written to a file -- and a file in the current
// directory is one nobody can find (the audit's R-07). Beside the executable is
// where init-error.txt has always gone, and it is a place a caller can name
// without guessing. Empty when the program's own path cannot be read, in which
// case the bare name is the best that can be done.
static String BesideProgram(PCTSTR ptzName)
{
	String dir = ProgramDirectory();

	return dir.empty() ? String(ptzName) : (dir + L"\\" + ptzName);
}

// Say what went wrong where it will still be there afterwards.
//
// A shipped image has no trace channel open this early, and the serial port
// belongs to the test rig rather than to the person in front of the machine. So
// the durable copy goes next to the executable -- X:\Windows\System32\init-error.txt
// in a PE -- which is beside the file CmdLine names, and therefore where someone
// looking at a boot that stopped will be.
VOID ReportStartupError(PCTSTR ptzMessage)
{
	if (g_hXLog)
	{
		SetLogs((PTSTR) ptzMessage, E_FAIL);
	}
	{
		String dir = ProgramDirectory();

		if (!dir.empty())
		{
			WriteCliFile((PTSTR) (dir + L"\\init-error.txt").c_str(),
				String(ptzMessage) + L"\n");
		}
	}
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// `phos --init [file]` -- the boot entry that names no path.
//
// The default is init.phos next to this executable, which in a PE is
// X:\Windows\System32\init.phos: a constant, so HKLM\SYSTEM\Setup\CmdLine can
// read "phos.exe --init" with no path and no %Variable% in it. An explicit
// path still wins, which is what a caller or the test rig uses to point the
// entry at a copy somewhere else.
//
// Returns TRUE when the line was an --init invocation. outScript then names the
// script to run, or is left empty when the file is missing, which the caller
// turns into a non-zero exit. The existence check lives here on purpose: Main
// ignores a load that failed and then sits in the tray, so a mistyped name used
// to mean a black screen with no explanation at all.
//////////////////////////////////////////////////////////////////////////////////////////////////
BOOL InitCli(PTSTR ptzCmdLine, String& outScript)
{
	std::vector<String> args;
	String exeDir = ProgramDirectory();
	String path;

	SplitCliArgs(ptzCmdLine, args);
	if (args.empty() || (UStrCmpI((PTSTR) args[0].c_str(), TEXT("--init")) != 0))
	{
		return FALSE;
	}

	if ((args.size() >= 2) && !args[1].empty())
	{
		path = args[1];
	}
	else if (!exeDir.empty())
	{
		path = exeDir + L"\\init.phos";
	}

	if (path.empty() || !UFileExist((PTSTR) path.c_str()))
	{
		String what = path.empty() ? String(L"init.phos (next to this executable, which could not be located)") : path;
		String message = String(L"phos --init: no such script: ") + what;

		ReportStartupError((PTSTR) message.c_str());
		return TRUE;
	}

	outScript = path;
	return TRUE;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// Anything the command line names that is not `--init`, `check`, `run` or
// `version`.
//
// There is no fallback to run: the text dialect was removed, so a line that it
// would have understood -- a script named by an extension association, a
// "SHUT R" left in a registry value by an older image -- is refused, and the
// refusal is written where it outlives the process. Naming the removed dialect
// is the point: the person reading init-error.txt is looking at a boot that
// stopped, and "unknown command" alone would not tell them why it used to work.
//////////////////////////////////////////////////////////////////////////////////////////////////
INT RefuseCommandLine(PTSTR ptzCmdLine)
{
	String message = String(L"phos: not a command line this program understands: ") + ptzCmdLine + L"\n"
		L"the text command dialect was removed; write a script and run it:\n"
		L"  phos.exe run <file.phos>\n"
		L"other forms: phos.exe --init [file] | check <file> | version\n";

	ReportStartupError((PTSTR) message.c_str());
	return 2;
}

// Where a script named on the command line is.
//
// A path that is already there is used as written. A relative one is looked for
// beside the executable *first*, which is the rule --init uses for init.phos and
// the one a registry verb needs: Explorer starts a verb's command line without a
// working directory anyone can predict, so
// "phos.exe run disposition\verbs\shutdown.phos" has to mean "beside the
// program". That order matters -- the other way round, a file planted in whatever
// directory the shell happened to start in would win, and these run with the
// rights to change the machine. A path that is rooted, or that has a drive on it,
// is never looked for anywhere else.
//
// Nothing found -- neither beside the program nor as written -- answers with the
// path it was given: the caller reports the name the user wrote, not the one we
// went looking for.
String ResolveScriptPath(const String& given)
{
	if (given.empty())
	{
		return given;
	}
	if ((given[0] == '\\') || (given[0] == '/') || (given.find(L':') != String::npos))
	{
		return given;       // rooted, or with a drive on it: taken as written
	}
	{
		String dir = ProgramDirectory();
		String beside = dir.empty() ? String() : (dir + L"\\" + given);

		if (!beside.empty() && UFileExist((PTSTR) beside.c_str()))
		{
			return beside;
		}
	}
	// Not beside the program: the path as written, so a relative path from a
	// command line still means what it looks like it means.
	return given;
}

BOOL IsCli(PTSTR ptzCmdLine, INT *piResult)
{
	std::vector<String> args;

	SplitCliArgs(ptzCmdLine, args);

	// Two spellings, on purpose. WinMain's command line leaves the program name
	// out, so anything that repeats it -- `phos.exe phos check x.phos` -- is one
	// token longer than it needs to be, and `phos.exe check x.phos` is what
	// anyone actually types. Accept both.
	//
	// Only these three words are ours. Anything else returns FALSE, and what is
	// left of the line is refused by name -- see RefuseCommandLine.
	if (!args.empty() && (UStrCmpI((PTSTR) args[0].c_str(), TEXT("phos")) == 0))
	{
		args.erase(args.begin());
	}
	if (args.empty())
	{
		return FALSE;
	}
	{
		bool known = (UStrCmpI((PTSTR) args[0].c_str(), TEXT("check")) == 0) ||
			(UStrCmpI((PTSTR) args[0].c_str(), TEXT("run")) == 0) ||
			(UStrCmpI((PTSTR) args[0].c_str(), TEXT("version")) == 0);

		if (!known)
		{
			return FALSE;
		}
	}

	if ((args.size() >= 2) && (UStrCmpI((PTSTR) args[0].c_str(), TEXT("check")) == 0))
	{
		String err;
		String out = (args.size() >= 3) ? args[2] : (args[1] + L".check.txt");
		bool ok = Lang::CheckFile((PCTSTR) ResolveScriptPath(args[1]).c_str(), err);

		WriteCliFile((PCTSTR) out.c_str(), ok ? String(L"OK\n") : (String(L"ERROR ") + err + L"\n"));
		*piResult = ok ? 0 : 1;
		return TRUE;
	}
	if ((args.size() >= 2) && (UStrCmpI((PTSTR) args[0].c_str(), TEXT("run")) == 0))
	{
		// LoadFile answers with a Win32 code when it fails, and those are
		// positive. SUCCEEDED() only looks at the sign bit, so "phos run
		// missing.phos" used to exit 0 and every caller -- proc.run among them
		// -- was told the script had run.
		*piResult = (LoadFile((PTSTR) ResolveScriptPath(args[1]).c_str()) == S_OK) ? 0 : 1;
		return TRUE;
	}
	if (UStrCmpI((PTSTR) args[0].c_str(), TEXT("version")) == 0)
	{
		WriteCliFile((PCTSTR) BesideProgram(TEXT("phos-version.txt")).c_str(),
			String(L"phos ") + STR_Version + L"\n");
		*piResult = 0;
		return TRUE;
	}

	WriteCliFile((PCTSTR) BesideProgram(TEXT("phos-usage.txt")).c_str(),
		L"usage:\n  phos check <file.phos> [outfile]\n  phos run <file.phos>\n  phos version\n");
	*piResult = 2;
	return TRUE;
}
//////////////////////////////////////////////////////////////////////////////////////////////////
