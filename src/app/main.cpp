INT APIENTRY _tWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PTSTR ptzCmdLine, INT iShowCmd)
{
	INT iResult = S_OK;	
	g_hInst = hInstance;
	XLogStartFromEnv();
	CurrentProcess = GetCurrentProcess();

	// `phos --init` is the boot entry: it resolves the script itself, so the
	// registry CmdLine can be a constant with no path and no %Variable% in it.
	// The synthesized "Main <file>" line then goes through the ordinary path
	// below, which is what makes the desktop switch / tray / MainThread
	// keep-alive happen. Declared out here because ptzCmdLine will point into it.
	Lang::String initLine;
	{
		BOOL InitCli(PTSTR ptzCmdLine, Lang::String& outLine);

		if (ptzCmdLine[0] && InitCli(ptzCmdLine, initLine))
		{
			if (initLine.empty())
			{
				// The script was named (or defaulted to) and is not there. The
				// init helper has already recorded which path; stop here rather
				// than hand Main a file it cannot load, because Main ignores that
				// and then sits in the tray -- a black screen that says nothing.
				return 1;
			}
			ptzCmdLine = (PTSTR) initLine.c_str();
		}
	}

	// `phos check` and `phos run` are handled here, before the text
	// command path sees the line. The helpers are defined at the end of this
	// file; the declaration is repeated because this body comes first.
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
		NotChineseLangID = (GetSystemDefaultLangID() - 0x0804);
		g_iShowCmd = iShowCmd;		
		UINT16ToUStr((PTSTR)LogoDescText,UDescStr,_NumOf(UDescStr));
		SetError(NULL);
		iResult = Command(ptzCmdLine);		
		InstallHookKeyBoard(FALSE);
		if (g_bInit)
		{
			SetMeOnTopMenu();
		}
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
// `phos --init [file]` -- the boot entry that names no path.
//
// The default is init.phos next to this executable, which in a PE is
// X:\Windows\System32\init.phos: a constant, so HKLM\SYSTEM\Setup\CmdLine can
// read "phos.exe --init" with no path and no %Variable% in it. An explicit
// path still wins, which is what a caller uses to point the entry at a copy
// somewhere else.
//
// Returns TRUE when the line was an --init invocation. outLine then holds the
// text-form `Main "<file>"` line, or is left empty when the file is missing,
// which the caller turns into a non-zero exit. The existence check lives here on
// purpose: Main ignores a load that failed and then sits in the tray, so a
// mistyped name would mean a black screen with no explanation at all.
//////////////////////////////////////////////////////////////////////////////////////////////////
BOOL InitCli(PTSTR ptzCmdLine, String& outLine)
{
	std::vector<String> args;
	TCHAR tzExe[MAX_PATH];
	String exeDir;
	String path;

	SplitCliArgs(ptzCmdLine, args);
	if (args.empty() || (UStrCmpI((PTSTR) args[0].c_str(), TEXT("--init")) != 0))
	{
		return FALSE;
	}

	{
		DWORD uLen = GetModuleFileName(NULL, tzExe, MAX_PATH);
		String s = ((uLen > 0) && (uLen < MAX_PATH)) ? String(tzExe) : String();
		size_t slash = s.find_last_of(L"\\/");

		if (slash != String::npos)
		{
			exeDir = s.substr(0, slash);
		}
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
		TCHAR tzBuf[MAX_STR];
		String what = path.empty() ? String(L"init.phos (next to this executable, which could not be located)") : path;

		UStrPrint(tzBuf, TEXT("phos --init: no such script: %s"), what.c_str());
		if (g_hXLog)
		{
			SetLogs(tzBuf, E_FAIL);
		}
		if (!exeDir.empty())
		{
			// A durable copy on purpose. In a shipped image no trace channel is
			// open this early, and a silent black boot is the one failure this
			// entry must not have: the file is left next to the executable that
			// CmdLine names, which is where someone would look.
			WriteCliFile((PTSTR) (exeDir + L"\\init-error.txt").c_str(), String(tzBuf) + L"\n");
		}
		return TRUE;
	}

	outLine = String(TEXT("Main \"")) + path + TEXT("\"");
	return TRUE;
}

BOOL IsCli(PTSTR ptzCmdLine, INT *piResult)
{
	std::vector<String> args;

	SplitCliArgs(ptzCmdLine, args);
	if (args.empty() || (UStrCmpI((PTSTR) args[0].c_str(), TEXT("phos")) != 0))
	{
		return FALSE;
	}

	if ((args.size() >= 3) && (UStrCmpI((PTSTR) args[1].c_str(), TEXT("check")) == 0))
	{
		String err;
		String out = (args.size() >= 4) ? args[3] : (args[2] + L".check.txt");
		bool ok = Lang::CheckFile((PCTSTR) args[2].c_str(), err);

		WriteCliFile((PCTSTR) out.c_str(), ok ? String(L"OK\n") : (String(L"ERROR ") + err + L"\n"));
		*piResult = ok ? 0 : 1;
		return TRUE;
	}
	if ((args.size() >= 3) && (UStrCmpI((PTSTR) args[1].c_str(), TEXT("run")) == 0))
	{
		*piResult = SUCCEEDED(LoadFile((PTSTR) args[2].c_str())) ? 0 : 1;
		return TRUE;
	}
	if ((args.size() >= 2) && (UStrCmpI((PTSTR) args[1].c_str(), TEXT("version")) == 0))
	{
		WriteCliFile(TEXT("phos-version.txt"), String(L"phos ") + STR_Version + L"\n");
		*piResult = 0;
		return TRUE;
	}

	WriteCliFile(TEXT("phos-usage.txt"),
		L"usage:\n  phos check <file.phos> [outfile]\n  phos run <file.phos>\n  phos version\n");
	*piResult = 2;
	return TRUE;
}
//////////////////////////////////////////////////////////////////////////////////////////////////
