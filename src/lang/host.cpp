//////////////////////////////////////////////////////////////////////////////////////////////////
// Phosphor host functions.
//
// A verb and the text command with the same job reach the same implementation
// through the capability layer below, not through a command line: a script
// calls FsDelete() where the dialect's FILE did, and the two meet inside
// platform/. Nothing here is text any more: a hotkey binding is a closure the
// interpreter keeps alive, not a command line handed to another process.
//
// The exceptions are the things no text command covers: file attributes,
// directory listing, environment reads and the path record. Those talk to
// Win32 directly and say so in their comments.
//////////////////////////////////////////////////////////////////////////////////////////////////
#include "../core/encoding.h"

namespace Lang
{
#ifdef PHOS_TESTHOOKS
	// The one definition of the live-Env counter declared in runtime.h. It has
	// to sit at namespace scope, not inside the anonymous one below: the member
	// belongs to Lang::Env.
	volatile LONG Env::live = 0;
	// And the one definition of the deep-copy counter.
	volatile LONG g_deepCopies = 0;
#endif

	namespace
	{
		String ArgStr(const std::vector<Value>& args, size_t i, int line, const wchar_t *what)
		{
			if (i >= args.size())
			{
				throw Error(0x80070057, String(L"missing argument: ") + what, line);
			}
			if (args[i].kind != VK_STR)
			{
				throw Error(0x80070057, String(L"expected a string for ") + what, line);
			}
			return args[i].s;
		}

		long long ArgInt(const std::vector<Value>& args, size_t i, int line, const wchar_t *what)
		{
			if (i >= args.size())
			{
				throw Error(0x80070057, String(L"missing argument: ") + what, line);
			}
			if (args[i].kind != VK_INT)
			{
				throw Error(0x80070057, String(L"expected an integer for ") + what, line);
			}
			return args[i].i;
		}

		bool ArgBool(const std::vector<Value>& args, size_t i, int line, const wchar_t *what)
		{
			if (i >= args.size())
			{
				throw Error(0x80070057, String(L"missing argument: ") + what, line);
			}
			if (args[i].kind != VK_BOOL)
			{
				throw Error(0x80070057, String(L"expected true or false for ") + what, line);
			}
			return args[i].b;
		}

		const Value *Field(const std::vector<Value>& args, size_t i, const wchar_t *key)
		{
			if ((i >= args.size()) || (args[i].kind != VK_RECORD) || !args[i].rec)
			{
				return NULL;
			}
			for (size_t k = 0; k < args[i].rec->size(); k++)
			{
				if ((*args[i].rec)[k].first == key)
				{
					return &(*args[i].rec)[k].second;
				}
			}
			return NULL;
		}

		bool OptBool(const std::vector<Value>& args, size_t i, const wchar_t *key, bool dflt)
		{
			const Value *v = Field(args, i, key);
			if (!v)
			{
				return dflt;
			}
			return Truthy(*v);
		}

		String OptStr(const std::vector<Value>& args, size_t i, const wchar_t *key)
		{
			const Value *v = Field(args, i, key);
			if ((!v) || (v->kind != VK_STR))
			{
				return L"";
			}
			return v->s;
		}

		// `{ optional: true }` in the trailing options record turns a failing
		// verb into a traced warning instead of an abort. It is the one explicit
		// way to say "this step may not be there"; the default stays abort.
		bool IsOptional(const std::vector<Value>& args)
		{
			if (args.empty())
			{
				return false;
			}
			{
				const Value *v = Field(args, args.size() - 1, L"optional");
				return v && Truthy(*v);
			}
		}

		// A short pointer at which call a verb was given, so an optional failure
		// in the trace says *what* failed and not only which verb. Without it a
		// reader sees "proc.run (optional) <2>" and has to count lines.
		String BriefArg(const std::vector<Value>& args)
		{
			if (args.empty() || (args[0].kind != VK_STR))
			{
				return String();
			}
			{
				String s = args[0].s;

				if (s.size() > 96)
				{
					s = s.substr(0, 93) + L"...";
				}
				return s;
			}
		}

		Value MakeStr(const String& s)
		{
			Value v;
			v.kind = VK_STR;
			v.s = s;
			return v;
		}

		Value MakeInt(long long i)
		{
			Value v;
			v.kind = VK_INT;
			v.i = i;
			return v;
		}

		Value MakeNil()
		{
			return Value();
		}

		Value MakeBool(bool b)
		{
			Value v;
			v.kind = VK_BOOL;
			v.b = b;
			return v;
		}

		// Add one field to a record that is being built. Fields are looked up by
		// name, so the order here is only for reading the code.
		void RecPut(Value& rec, const wchar_t *key, const Value& v)
		{
			rec.rec->push_back(std::make_pair(String(key), v));
		}

		// A FILETIME as seconds since 1970, so two times can be compared and
		// subtracted. FILETIME counts 100 ns ticks from 1601; the two epochs are
		// 11644473600 seconds apart.
		long long FileTimeToEpoch(const FILETIME& ft)
		{
			ULARGE_INTEGER u;

			u.LowPart = ft.dwLowDateTime;
			u.HighPart = ft.dwHighDateTime;
			if (u.QuadPart < 116444736000000000ULL)
			{
				return 0;		// before 1970; nothing in a PE is, but do not wrap
			}
			return (long long) ((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
		}

		// Append one argument to a command line, quoted the way
		// CommandLineToArgvW will parse it back.
		//
		// An argument list that is joined into a line and then split again is
		// the whole game: get this wrong and the two ends disagree about where
		// one argument ends and the next begins. The old version only quoted
		// when it saw a space, so an argument containing a quote went through
		// unescaped, an empty argument disappeared, and a path that ended in a
		// backslash -- "X:\Program Files\Dir\" -- produced `...\Dir\"` where the
		// final quote was escaped by that backslash and the argument swallowed
		// whatever followed it.
		//
		// The rules, from the documentation for parsing a C command line,
		// applied backwards:
		//   * an empty argument, or one with a space, a tab or a quote, needs
		//     quotes;
		//   * a backslash escapes the character after it only when that is a
		//     quote, so a backslash run that walks a quote doubles, plus one;
		//   * a backslash run at the end of a quoted argument doubles, because
		//     the closing quote must not be escaped by it.
		void AppendArg(String& line, const String& arg, bool bForce)
		{
			bool bQuote = bForce || arg.empty() ||
				(arg.find_first_of(L" \t\"") != String::npos);

			if (!bQuote)
			{
				line += arg;
				return;
			}

			size_t backslashes = 0;

			line += L'"';
			for (size_t i = 0; i < arg.size(); i++)
			{
				if (arg[i] == L'\\')
				{
					backslashes++;
					continue;
				}
				if (arg[i] == L'"')
				{
					line.append(backslashes * 2 + 1, L'\\');
					line += L'"';
					backslashes = 0;
					continue;
				}
				line.append(backslashes, L'\\');
				backslashes = 0;
				line += arg[i];
			}
			line.append(backslashes * 2, L'\\');
			line += L'"';
		}

		// Quote for CreateProcess / the legacy parsers: only when the text needs
		// it, so a plain X:\Windows\... stays readable in the trace.
		String Quote(const String& s)
		{
			String out;

			AppendArg(out, s, false);
			return out;
		}

		// CreateProcess gets the whole line as lpCommandLine with no
		// lpApplicationName, and " X:\Program Files\x.exe" (a quoted token after
		// a leading space) is not parsed the way it looks: the space is part of
		// the first token, the quote is not at the start of the string, and the
		// call fails. Always quote the program and put it straight after the
		// EXEC flags, which is the legacy form the shipped config uses.
		String QuoteAlways(const String& s)
		{
			String out;

			AppendArg(out, s, true);
			return out;
		}

		// A Lua string literal for the -code argument.
		//
		// The value used to be spliced straight into a [[...]] long bracket or a
		// '...' quote, so a value carrying ]] or ' ended the string and the rest
		// of it was Lua, running inside WinXShell. Quoting and escaping is the
		// fix: whatever the script passed stays data. A control character has no
		// useful spelling here and is refused rather than quietly dropped.
		String LuaQuote(const String& s, int line, const wchar_t *what)
		{
			String out = L"\"";

			for (size_t i = 0; i < s.size(); i++)
			{
				wchar_t ch = s[i];

				if (ch == L'\\')
				{
					out += L"\\\\";
				}
				else if (ch == L'"')
				{
					out += L"\\\"";
				}
				else if (ch < 0x20)
				{
					throw Error(0x80070057,
						String(L"the ") + what + L" contains a control character", line);
				}
				else
				{
					out += ch;
				}
			}
			out += L"\"";
			return out;
		}

		// Does this verb change something? Its own declared grade says so, so
		// the answer cannot drift away from the table the way a second list can.
		//
		void TraceVerb(const String& name, int line, HRESULT hr)
		{
			if (g_hXLog)
			{
				TCHAR tzBuf[MAX_STR];
				g_ctx.uLineNo = (UINT) line;
				UStrPrint(tzBuf, TEXT("%s"), name.c_str());
				SetLogs(tzBuf, hr);
			}
		}

		String EnvValue(const wchar_t *name)
		{
			DWORD uLen = GetEnvironmentVariableW(name, NULL, 0);
			if (uLen == 0)
			{
				return L"";
			}
			{
				std::vector<wchar_t> buf(uLen + 1, 0);
				GetEnvironmentVariableW(name, buf.data(), uLen + 1);
				return String(buf.data());
			}
		}

		// --- log ------------------------------------------------------------------

		Value HostLogOpen(Interp&, std::vector<Value>& args, int line)
		{
			String target = ArgStr(args, 0, line, L"target");
			std::vector<TCHAR> buf(target.begin(), target.end());
			buf.push_back(0);
			LogOpen(buf.data(), 16);
			return MakeNil();
		}

		Value HostLogClose(Interp&, std::vector<Value>&, int line)
		{
			LogClose();
			return MakeNil();
		}

		Value HostLogText(Interp&, std::vector<Value>& args, int line, const wchar_t *kind)
		{
			if (g_hXLog)
			{
				TCHAR tzBuf[MAX_STR];
				String msg = (args.size() > 0) ? ToDisplay(args[0]) : String();
				g_ctx.uLineNo = (UINT) line;
				UStrPrint(tzBuf, TEXT("%s %s"), kind, msg.c_str());
				SetLogs(tzBuf, S_OK);
			}
			return MakeNil();
		}

		Value HostLogInfo(Interp& in, std::vector<Value>& args, int line) { return HostLogText(in, args, line, TEXT("info")); }
		Value HostLogWarn(Interp& in, std::vector<Value>& args, int line) { return HostLogText(in, args, line, TEXT("warn")); }
		Value HostLogError(Interp& in, std::vector<Value>& args, int line) { return HostLogText(in, args, line, TEXT("error")); }

		// fail(message, {code}) -- how a script says "stop, and here is why".
		//
		// Without it a script can only wait for a host verb to fail; it has no
		// way to report a condition it worked out itself. The default code is
		// E_FAIL rather than zero so a record always carries one.
		Value HostFail(Interp&, std::vector<Value>& args, int line)
		{
			String message = ArgStr(args, 0, line, L"message");
			const Value *code = Field(args, 1, L"code");
			HRESULT hr = E_FAIL;

			if (code && (code->kind == VK_INT))
			{
				hr = (HRESULT) code->i;
			}
			TraceVerb(String(L"fail ") + message, line, hr);
			throw Error(hr, message, line);
		}

		// Which desktop is this process on, and which one is being displayed?
		// "The shell is running but nothing is visible" is exactly this question,
		// and it is the one thing a screenshot cannot answer.
		Value HostDebugDesktop(Interp&, std::vector<Value>&, int line)
		{
			TCHAR tzNames[MAX_NAME];
			TCHAR tzLine[MAX_STR];

			DesktopNamesText(tzNames, _NumOf(tzNames));
			UStrPrint(tzLine, TEXT("debug.desktop %s"), tzNames);
			TraceVerb(tzLine, line, S_OK);
			return MakeStr(tzNames);
		}

#ifdef PHOS_TESTHOOKS
		// How many Env objects are alive, for the test that says a closure must
		// not keep its defining scope alive for ever. Test builds only.
		Value HostDebugEnvCount(Interp&, std::vector<Value>&, int line)
		{
			TraceVerb(L"debug.env_count", line, S_OK);
			return MakeInt((long long) Env::live);
		}

		// How many deep copies have been made, for the test that says handing a
		// large list to a function must not copy it. Test builds only.
		Value HostDebugCopyCount(Interp&, std::vector<Value>&, int line)
		{
			TraceVerb(L"debug.copy_count", line, S_OK);
			return MakeInt((long long) g_deepCopies);
		}

		// Shorten the step budget for the rest of this entry, so a case can test
		// the watchdog in a few steps instead of twenty million. The argument is
		// how much budget is left, counted from here rather than from the start
		// of the script: a case cannot know how many steps it has already spent.
		// Test builds only.
		Value HostDebugStepLimit(Interp& in, std::vector<Value>& args, int line)
		{
			long long n = ArgInt(args, 0, line, L"steps");

			if (n < 1)
			{
				throw Error(0x80070057, L"debug.step_limit: steps must be positive", line);
			}
			in.stepLimit = in.steps + (UINT64) n;
			TraceVerb(L"debug.step_limit", line, S_OK);
			return MakeNil();
		}

		// Ask for a specific death, on purpose, so the rig can prove the crash
		// recorder works. Only exists in a Release-Test build.
		//
		// It is a verb rather than only a command line switch because changing
		// the image's Setup\CmdLine needs an elevated hive rewrite, while the
		// rig already has a path that injects a script without one.
		Value HostDebugCrash(Interp&, std::vector<Value>& args, int line)
		{
			String kind = ArgStr(args, 0, line, L"kind");

			TraceVerb(String(L"debug.crash ") + kind, line, S_OK);
			CrashTestTrigger(kind.c_str());

			// The trigger returns only if the kind was not recognised.
			throw Error(0x80070057, L"debug.crash: unknown kind (av, div, abort)", line);
		}
#endif

		// --- proc -----------------------------------------------------------------

		// Starting a program does not go through the EXEC text command.
		//
		// It used to, and that was wrong for a reason worth writing down:
		// Command() trims a fully quoted argument, but its check is "the first
		// character is a quote -> drop it, then drop the last character if that
		// is also a quote". A normal command line of the form
		//   "X:\Program Files\App\App.exe" -arg
		// therefore came out as
		//   X:\Program Files\App\App.exe" -arg
		// with the opening quote gone and the closing one left behind, and
		// CreateProcess answered 2. The shipped config never noticed because it
		// always wrote a flag first (`EXEC !"..."` / `EXEC -hide "..."`); the
		// moment the structured chain asked for a plain launch, it broke.
		//
		// Nothing below is parsed as text, so that class of bug is gone.
		struct ProcSpec
		{
			String cmd;		// exactly what CreateProcess gets, for the trace
			String wd;		// working directory, empty means inherit
			bool   hidden;
			// NAME=VALUE pairs to add to (or replace in) the child's
			// environment. Empty means "inherit, do not touch".
			std::vector<std::pair<String, String> > env;
			ProcSpec() : hidden(false) {}
		};

		ProcSpec MakeProcSpec(const String& program, const std::vector<Value>& args, size_t optsAt)
		{
			ProcSpec spec;
			const Value *list = Field(args, optsAt, L"args");

			spec.cmd = QuoteAlways(program);
			spec.hidden = OptBool(args, optsAt, L"hidden", false);
			spec.wd = OptStr(args, optsAt, L"wd");
			{
				// { env: { NAME: "value" } } adds to (or replaces in) the child's
				// environment. The rest of ProcSpec carried this from the start;
				// what was missing was the way for a script to say it.
				const Value *env = Field(args, optsAt, L"env");

				if (env && (env->kind == VK_RECORD) && env->rec)
				{
					for (size_t i = 0; i < env->rec->size(); i++)
					{
						spec.env.push_back(std::make_pair(
							(*env->rec)[i].first, ToDisplay((*env->rec)[i].second)));
					}
				}
			}
			if (list && (list->kind == VK_LIST) && list->list)
			{
				for (size_t i = 0; i < list->list->size(); i++)
				{
					spec.cmd += L' ';
					spec.cmd += Quote(ToDisplay((*list->list)[i]));
				}
			}
			return spec;
		}

		// The process handle is handed back rather than closed, because
		// proc.spawn keeps it so a later proc.await can wait on it. Callers that
		// are not going to wait pass NULL and the handle is closed here.
		// The parent's environment with the given NAME=VALUE pairs applied, in the
		// sorted, double-NUL terminated form CreateProcess wants.
		std::vector<TCHAR> BuildEnv(const std::vector<std::pair<String, String> >& over)
		{
			std::map<String, String> merged;
			LPTSTR p = GetEnvironmentStrings();

			if (p)
			{
				for (LPTSTR q = p; *q; )
				{
					String entry = q;
					size_t eq = entry.find(L'=');

					// The "=C:=C:\..." entries have an empty name and are
					// deliberately dropped: they are per-drive cwd markers, not
					// variables, and CreateProcess does not want them.
					if ((eq != String::npos) && (eq > 0))
					{
						merged[entry.substr(0, eq)] = entry.substr(eq + 1);
					}
					q += entry.size() + 1;
				}
				FreeEnvironmentStrings(p);
			}
			for (size_t i = 0; i < over.size(); i++)
			{
				merged[over[i].first] = over[i].second;
			}

			std::vector<TCHAR> out;
			for (std::map<String, String>::const_iterator it = merged.begin(); it != merged.end(); ++it)
			{
				String line = it->first + L"=" + it->second;

				out.insert(out.end(), line.begin(), line.end());
				out.push_back(0);
			}
			out.push_back(0);
			return out;
		}

		// dwTimeout only means anything when wait is set. piExit, when it is
		// given, receives the child's exit code -- the one thing a script could
		// not find out before, which made a failing drvload or diskpart look
		// exactly like one that worked.
		HRESULT SpawnProcess(const ProcSpec& spec, bool wait, HANDLE *phProcess, DWORD *pdwPid,
			DWORD dwTimeout, int *piExit)
		{
			STARTUPINFO si = {0};
			PROCESS_INFORMATION pi;
			std::vector<TCHAR> buf(spec.cmd.begin(), spec.cmd.end());
			std::vector<TCHAR> env;
			DWORD dwFlags = 0;
			LPVOID lpEnv = NULL;

			buf.resize(spec.cmd.size() + 1, 0);
			if (!spec.env.empty())
			{
				env = BuildEnv(spec.env);
				lpEnv = env.empty() ? NULL : (LPVOID) env.data();
				dwFlags |= CREATE_UNICODE_ENVIRONMENT;
			}
			si.cb = sizeof(si);
			si.dwFlags = STARTF_USESHOWWINDOW;
			si.wShowWindow = spec.hidden ? SW_HIDE : SW_NORMAL;
			si.lpDesktop = TEXT("WinSta0\\Default");

			if (!CreateProcess(NULL, buf.data(), NULL, NULL, FALSE, dwFlags, lpEnv,
				spec.wd.empty() ? NULL : spec.wd.c_str(), &si, &pi))
			{
				return (HRESULT) GetLastError();
			}
			if (wait)
			{
				// A timeout rather than INFINITE. A child that never returns
				// used to hang the whole chain behind it, and the screen stays
				// on whatever was drawn last, which is the failure this project
				// keeps saying it will not have.
				DWORD dwWaited = WaitPumping(pi.hProcess, dwTimeout);

				if (dwWaited == WAIT_TIMEOUT)
				{
					// Left running on purpose: ending it is the script's
					// decision, and the handle it would need is not handed
					// back on this path.
					CloseHandle(pi.hThread);
					CloseHandle(pi.hProcess);
					return (HRESULT) ERROR_TIMEOUT;
				}
				if (piExit)
				{
					DWORD dwExit = 0;

					if (GetExitCodeProcess(pi.hProcess, &dwExit))
					{
						*piExit = (int) dwExit;
					}
				}
			}
			SetProcessWorkingSetSize(CurrentProcess, (SIZE_T) -1, (SIZE_T) -1);
			CloseHandle(pi.hThread);
			if (phProcess)
			{
				*phProcess = pi.hProcess;
				if (pdwPid)
				{
					*pdwPid = pi.dwProcessId;
				}
			}
			else
			{
				CloseHandle(pi.hProcess);
			}
			return S_OK;
		}

		// Runs it, records it, and raises on failure so `optional: true` and
		// try/catch have something to work with.
		void SpawnAndTrace(const String& verb, const ProcSpec& spec, bool wait, int line,
			HANDLE *phProcess, DWORD *pdwPid,
			DWORD dwTimeout = INFINITE, int *piExit = NULL)
		{
			HRESULT hr = SpawnProcess(spec, wait, phProcess, pdwPid, dwTimeout, piExit);

			TraceVerb(verb + L" " + spec.cmd, line, hr);
			// hr != S_OK, not FAILED(hr): a failed CreateProcess reports
			// GetLastError(), and a Win32 error code is *positive* (2, 87), so
			// FAILED() - which only looks at the sign bit - says "fine". That is
			// how two regedit launches against a PE with no regedit.exe came out
			// as trace lines nobody acted on.
			if (hr != S_OK)
			{
				TCHAR tzBuf[MAX_STR];

				if (hr == (HRESULT) ERROR_TIMEOUT)
				{
					UStrPrint(tzBuf, TEXT("%s did not finish inside its timeout"), spec.cmd.c_str());
				}
				else
				{
					UStrPrint(tzBuf, TEXT("%s failed (0x%08X)"), spec.cmd.c_str(), (UINT) hr);
				}
				throw Error(hr, tzBuf, line);
			}
		}

		// --- per-task trails ------------------------------------------------------

		// Where a task's own trail goes. Next to the parent's log when that is a
		// file; in the temp directory when the parent writes to a device, because
		// a device like COM1 cannot be handed to a second writer.
		String ChildTrailPath(const String& script)
		{
			size_t cut = script.find_last_of(L"\\/");
			String stem = (cut == String::npos) ? script : script.substr(cut + 1);
			String base = LogFileName;

			if (base.empty() || ((base.size() >= 2) && (base[0] == L'\\') && (base[1] == L'\\')))
			{
				TCHAR tzTemp[MAX_PATH];
				DWORD n = GetTempPath(MAX_PATH, tzTemp);
				String dir = (n > 0) ? String(tzTemp) : String();

				while (!dir.empty() && (dir[dir.size() - 1] == L'\\'))
				{
					dir.erase(dir.size() - 1);
				}
				return (dir.empty() ? String(L"phos-task") : dir + L"\\phos-task") +
					L"-" + stem + L".log";
			}
			return base + L"." + stem + L".task";
		}

		// Append text to the current channel exactly as written. Used to copy a
		// finished task's trail back into the parent's log: those lines already
		// carry a line number and a timestamp from the task's own clock, so
		// re-formatting them would stack a second header on each.
		void AppendRawLog(const String& text)
		{
			if (!g_hXLog)
			{
				return;
			}
			LogAppend(text.c_str());
			if (g_bXLogStream)
			{
				XLogWrite(TRUE);
			}
		}

		// Copy a finished task's own trail into the parent's channel, then drop
		// the file. Called wherever a task is awaited, so the log reads in the
		// order tasks finished rather than in the order they started.
		void FlushTaskLog(Interp& in, long long pid)
		{
			std::map<long long, String>::iterator it = in.procLogs.find(pid);

			if (it == in.procLogs.end())
			{
				return;
			}
			{
				String path = it->second;
				String label = path;
				size_t cut = label.find_last_of(L"\\/");
				HANDLE h = CreateFile(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
					OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

				if (cut != String::npos)
				{
					label = label.substr(cut + 1);
				}
				in.procLogs.erase(it);
				if (h != INVALID_HANDLE_VALUE)
				{
					DWORD size = GetFileSize(h, NULL);
					std::vector<wchar_t> raw(((size_t) size / sizeof(wchar_t)) + 2, 0);
					DWORD got = 0;

					if ((size > 0) && (size != INVALID_FILE_SIZE) &&
						ReadFile(h, raw.data(), size, &got, NULL))
					{
						String text(raw.data());

						if (!text.empty() && (text[0] == 0xFEFF))
						{
							text.erase(0, 1);
						}
						for (size_t at = 0; at < text.size(); )
						{
							size_t nl = text.find(L'\n', at);
							String line = text.substr(at, (nl == String::npos) ? String::npos : nl - at);

							at = (nl == String::npos) ? text.size() : nl + 1;
							if (!line.empty() && (line[line.size() - 1] == L'\r'))
							{
								line.erase(line.size() - 1);
							}
							if (!line.empty())
							{
								// Marked so a reader can tell a copied line from one
								// the parent wrote: the timestamp in it belongs to
								// the task's clock, which started at zero, so the
								// two must not be read as one timeline.
								AppendRawLog(L"task " + label + L": " + line + L"\n");
							}
						}
					}
					CloseHandle(h);
				}
				DeleteFile(path.c_str());
			}
		}

		// A process value is a record { pid: N }. Nothing native crosses into the
		// language: the interpreter keeps the handles, the script keeps a number.
		Value MakeProcValue(DWORD dwPid)
		{
			Value v;

			v.kind = VK_RECORD;
			v.rec = RecordPtr(new std::vector<std::pair<String, Value> >());
			v.rec->push_back(std::make_pair(String(L"pid"), MakeInt((long long) dwPid)));
			return v;
		}

		bool ProcPid(const std::vector<Value>& args, size_t i, long long *pid, int line)
		{
			if ((i < args.size()) && (args[i].kind == VK_RECORD) && args[i].rec)
			{
				for (size_t k = 0; k < args[i].rec->size(); k++)
				{
					if ((*args[i].rec)[k].first == L"pid" && ((*args[i].rec)[k].second.kind == VK_INT))
					{
						*pid = (*args[i].rec)[k].second.i;
						return true;
					}
				}
			}
			throw Error(0x80070057, L"expected a process value from proc.spawn", line);
		}

		DWORD ProcTimeout(const std::vector<Value>& args, size_t optsAt)
		{
			const Value *v = Field(args, optsAt, L"timeout");

			if (v && (v->kind == VK_INT) && (v->i > 0))
			{
				return (DWORD) v->i;
			}
			return INFINITE;
		}

		Value HostProcRun(Interp&, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"program");
			int iExit = 0;

			// The answer is the child's exit code, not nil. Callers that do not
			// want it still do not have to look at it, and the ones that do --
			// anything that runs drvload, diskpart or wpeinit -- can now tell a
			// failure from a success. {timeout: <ms>} bounds the wait; with no
			// timeout it waits as long as it always did.
			SpawnAndTrace(L"proc.run", MakeProcSpec(path, args, 1), true, line, NULL, NULL,
				ProcTimeout(args, 1), &iExit);
			return MakeInt(iExit);
		}

		Value HostProcSpawn(Interp& in, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"program");
			HANDLE hProcess = NULL;
			DWORD dwPid = 0;

			SpawnAndTrace(L"proc.spawn", MakeProcSpec(path, args, 1), false, line, &hProcess, &dwPid);

			// A handle is only kept when it can still be awaited later; past
			// MAXIMUM_WAIT_OBJECTS an await_all could not take it anyway, so it
			// is closed instead of leaked.
			if (in.procs.size() < MAXIMUM_WAIT_OBJECTS)
			{
				in.procs[(long long) dwPid] = hProcess;
			}
			else
			{
				CloseHandle(hProcess);
			}
			return MakeProcValue(dwPid);
		}

		Value HostProcAwait(Interp& in, std::vector<Value>& args, int line)
		{
			long long pid = 0;

			ProcPid(args, 0, &pid, line);
			std::map<long long, HANDLE>::iterator it = in.procs.find(pid);
			if (it == in.procs.end())
			{
				throw Error(0x80070002, L"proc.await: that process has already been awaited (or was never spawned here)", line);
			}
			{
				DWORD dwWait = WaitPumping(it->second, ProcTimeout(args, 1));
				DWORD dwCode = 0;

				if (dwWait == WAIT_TIMEOUT)
				{
					// Still running: keep the handle so a later await can retry.
					//
					// Its trace so far goes out anyway. The question a timeout
					// raises is "what was it doing", and the answer is in the log
					// the task has already written -- leaving it in the child's
					// file until the task finishes means a task that never
					// finishes is the one task nobody can see.
					FlushTaskLog(in, pid);
					TraceVerb(String(L"proc.await timeout pid=") + ToText(MakeInt(pid)), line, S_OK);
					return MakeNil();
				}
				GetExitCodeProcess(it->second, &dwCode);
				CloseHandle(it->second);
				in.procs.erase(it);
				FlushTaskLog(in, pid);
				TraceVerb(String(L"proc.await pid=") + ToText(MakeInt(pid)), line, S_OK);
				return MakeInt((long long) dwCode);
			}
		}

		Value HostProcAwaitAll(Interp& in, std::vector<Value>& args, int line)
		{
			if (args.empty() || (args[0].kind != VK_LIST) || !args[0].list)
			{
				throw Error(0x80070057, L"proc.await_all needs a list of process values", line);
			}

			std::vector<Value>& items = *args[0].list;
			std::vector<HANDLE> handles;
			std::vector<long long> pids;
			Value out;

			for (size_t i = 0; i < items.size(); i++)
			{
				long long pid = 0;

				ProcPid(items, i, &pid, line);
				if (in.procs.find(pid) == in.procs.end())
				{
					throw Error(0x80070002, L"proc.await_all: a process has already been awaited", line);
				}
				pids.push_back(pid);
				handles.push_back(in.procs[pid]);
			}

			out.kind = VK_LIST;
			out.list = ListPtr(new std::vector<Value>());
			if (handles.empty())
			{
				return out;
			}
			if (handles.size() > MAXIMUM_WAIT_OBJECTS)
			{
				throw Error(0x80070057, L"proc.await_all: too many processes for one wait", line);
			}

			if (WaitForMultipleObjects((DWORD) handles.size(), handles.data(), TRUE,
				ProcTimeout(args, 1)) == WAIT_TIMEOUT)
			{
				TraceVerb(L"proc.await_all timeout", line, S_OK);
				return MakeNil();
			}

			for (size_t i = 0; i < handles.size(); i++)
			{
				DWORD dwCode = 0;

				GetExitCodeProcess(handles[i], &dwCode);
				CloseHandle(handles[i]);
				in.procs.erase(pids[i]);
				FlushTaskLog(in, pids[i]);
				out.list->push_back(MakeInt((long long) dwCode));
			}
			TraceVerb(String(L"proc.await_all n=") + ToText(MakeInt((long long) handles.size())), line, S_OK);
			return out;
		}

		Value HostProcKill(Interp&, std::vector<Value>& args, int line)
		{
			String name = ArgStr(args, 0, line, L"name");
			// Exact: proc.kill("ex") must not take explorer.exe with it. The
			// text command KILL keeps the prefix match it has always had.
			HRESULT hr = KillProc(name.c_str(), TRUE);

			// S_FALSE means "nothing by that name was running". The legacy
			// command reports that as its result rather than as an error, and
			// the chain relies on tolerating it, so it is traced, not raised.
			TraceVerb(String(L"proc.kill ") + name, line, hr);
			return MakeNil();
		}

		// Is that process running? FindProc answers with the pid (0 when it is
		// not), and which process is alive is the one question a trace cannot
		// answer for you - "SHEL returned 0" says nothing about whether the shell
		// survived the next step.
		Value HostProcExists(Interp&, std::vector<Value>& args, int line)
		{
			String name = ArgStr(args, 0, line, L"process name");
			Value v;

			v.kind = VK_BOOL;
			// Exact for the same reason as proc.kill: "sv" answering yes
			// because svchost.exe is running is a wrong answer, not a loose
			// one.
			v.b = (FindProcEx((PCTSTR) name.c_str(), TRUE, NULL) != 0);
			TraceVerb(String(L"proc.exists ") + name, line, S_OK);
			return v;
		}

		// --- registry -------------------------------------------------------------

		// "HKLM\SOFTWARE\X": the root name is the first segment, everything
		// after it is the subkey. Splitting it here rather than in the platform
		// layer keeps that layer free of String and of the language's argument
		// shape -- the point of the exercise is that the capability does not
		// know what a "command line" looks like.
		void SplitRegKey(const String& key, HKEY *phRoot, String *pSubKey, int line)
		{
			size_t cut = key.find(L'\\');
			String root = (cut == String::npos) ? key : key.substr(0, cut);
			HKEY hRoot = RegRootFromName(root.c_str());

			if (!hRoot)
			{
				throw Error(0x80070057, L"reg: unknown root key: " + root, line);
			}
			*phRoot = hRoot;
			*pSubKey = (cut == String::npos) ? String() : key.substr(cut + 1);
		}

		Value HostRegSet(Interp&, std::vector<Value>& args, int line)
		{
			String key = ArgStr(args, 0, line, L"key");
			HKEY hRoot = NULL;
			String subKey;
			HRESULT hr;

			if (args.size() < 3)
			{
				throw Error(0x80070057, L"reg.set takes a key, a value name and a value", line);
			}
			SplitRegKey(key, &hRoot, &subKey, line);
			{
				String name = ToDisplay(args[1]);
				const Value& v = args[2];

				// The type follows the value: an int or a bool is a DWORD, a
				// string is a REG_SZ. There is no bytes kind in the language
				// yet, so REG_BINARY waits for one rather than being faked with
				// a string (which is how the old text form spelled it).
				if ((v.kind == VK_INT) || (v.kind == VK_BOOL))
				{
					DWORD dw = (v.kind == VK_BOOL) ? (v.b ? 1u : 0u) : (DWORD) v.i;
					hr = RegSetValue(hRoot, subKey.c_str(), name.c_str(), REG_DWORD, &dw, sizeof(dw));
				}
				else
				{
					String text = ToDisplay(v);
					hr = RegSetValue(hRoot, subKey.c_str(), name.c_str(), REG_SZ,
						(const void *) text.c_str(), (DWORD) ((text.size() + 1) * sizeof(TCHAR)));
				}
				if (hr != ERROR_SUCCESS)
				{
					throw Error((HRESULT) hr, String(L"reg.set failed for ") + key, line);
				}
			}
			TraceVerb(String(L"reg.set ") + key, line, S_OK);
			return MakeNil();
		}

		Value HostRegDelete(Interp&, std::vector<Value>& args, int line)
		{
			String key = ArgStr(args, 0, line, L"key");
			String name;
			HKEY hRoot = NULL;
			String subKey;
			HRESULT hr;

			if (args.size() < 2)
			{
				throw Error(0x80070057, L"reg.delete takes a key and a value name", line);
			}
			SplitRegKey(key, &hRoot, &subKey, line);
			name = ToDisplay(args[1]);
			hr = RegDeleteValue(hRoot, subKey.c_str(), name.c_str());
			if (hr != S_OK)
			{
				throw Error(hr, String(L"reg.delete failed for ") + key, line);
			}
			TraceVerb(String(L"reg.delete ") + key, line, S_OK);
			return MakeNil();
		}

		Value HostRegDeleteTree(Interp&, std::vector<Value>& args, int line)
		{
			String key = ArgStr(args, 0, line, L"key");
			BOOL bForce = OptBool(args, 1, L"force", false);
			HKEY hRoot = NULL;
			String subKey;
			HRESULT hr;

			SplitRegKey(key, &hRoot, &subKey, line);

			// Both refusals are said here rather than left to the platform
			// guard, which only has an HRESULT to answer with: the script needs
			// to know which rule it broke and what to write instead. The guard
			// keeps its own copy of both, because the layer that reaches
			// SHDeleteKey must not depend on its caller having checked.
			if (subKey.find(L'\\') == String::npos)
			{
				throw Error(0x80070057,
					String(L"reg.delete_tree: '") + key + L"' is a key directly under its root, and"
					L" deleting one of those takes the whole branch with it -- name a key at least"
					L" two levels down, like HKLM\\Software\\Vendor", line);
			}
			if (!bForce && RegIsLoadedHive(subKey.c_str()))
			{
				throw Error((long long) HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED),
					String(L"reg.delete_tree: '") + key + L"' is inside a hive that reg.load_hive"
					L" attached, so this reaches a real installation's file and a reboot will not"
					L" undo it; pass {force: true} if that is what you mean", line);
			}

			hr = RegDeleteTreeGuarded(hRoot, subKey.c_str(), bForce);
			if (hr != S_OK)
			{
				throw Error(hr, String(L"reg.delete_tree failed for ") + key, line);
			}
			TraceVerb(String(L"reg.delete_tree ") + key + (bForce ? L" (force)" : L""), line, S_OK);
			return MakeNil();
		}

		Value HostRegGet(Interp&, std::vector<Value>& args, int line)
		{
			String key = ArgStr(args, 0, line, L"key");
			String name;
			HKEY hRoot = NULL;
			String subKey;
			DWORD dwType = 0;
			DWORD cbData = 0;
			HRESULT hr;

			if (args.size() < 2)
			{
				throw Error(0x80070057, L"reg.get takes a key and a value name", line);
			}
			SplitRegKey(key, &hRoot, &subKey, line);
			name = ToDisplay(args[1]);

			// Size first, then the data -- RegQueryValueEx answers the size when
			// it is handed no buffer.
			hr = RegQueryValueRaw(hRoot, subKey.c_str(), name.c_str(), &dwType, NULL, &cbData);
			if (hr != S_OK)
			{
				// Absent is an answer, not a failure: the caller is asking
				// whether the value is there, and nil says no.
				TraceVerb(String(L"reg.get ") + key + L" (absent)", line, S_OK);
				return MakeNil();
			}
			{
				std::vector<BYTE> buf((size_t) cbData + sizeof(TCHAR) + 2, 0);

				hr = RegQueryValueRaw(hRoot, subKey.c_str(), name.c_str(), &dwType, &buf[0], &cbData);
				if (hr != S_OK)
				{
					throw Error(hr, String(L"reg.get failed for ") + key, line);
				}
				TraceVerb(String(L"reg.get ") + key, line, S_OK);
				if ((dwType == REG_DWORD) && (cbData >= sizeof(DWORD)))
				{
					DWORD dw = 0;

					memcpy(&dw, &buf[0], sizeof(DWORD));
					return MakeInt((long long) dw);
				}
				// Everything else is handed back as text: REG_SZ and
				// REG_EXPAND_SZ are already NUL terminated, and a value nobody
				// can render as text has no type in the language yet.
				return MakeStr(String((const wchar_t *) &buf[0]));
			}
		}

		Value HostRegLoadHive(Interp&, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"hive file");
			String mount = ArgStr(args, 1, line, L"mount name");
			HRESULT hr = RegLoadHive(path.c_str(), mount.c_str());

			if (hr != S_OK)
			{
				throw Error(hr, String(L"reg.load_hive could not load ") + path, line);
			}
			TraceVerb(String(L"reg.load_hive ") + mount, line, S_OK);
			return MakeNil();
		}

		Value HostRegUnloadHive(Interp&, std::vector<Value>& args, int line)
		{
			String mount = ArgStr(args, 0, line, L"mount name");
			HRESULT hr = RegUnloadHive(mount.c_str());

			if (hr != S_OK)
			{
				throw Error(hr, String(L"reg.unload_hive could not unmount ") + mount, line);
			}
			TraceVerb(String(L"reg.unload_hive ") + mount, line, S_OK);
			return MakeNil();
		}

		// --- fs -------------------------------------------------------------------

		Value HostFsExists(Interp&, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"path");
			Value v;
			DWORD attr = GetFileAttributesW(path.c_str());

			v.kind = VK_BOOL;
			v.b = (attr != INVALID_FILE_ATTRIBUTES);
			return v;
		}

		// The one listing both verbs are: the caller says which kind of entry it
		// wants, and everything else -- the pattern, the sort, the fact that the
		// result is absolute paths -- is the same. Two verbs rather than one with
		// a mode switch, because a mode switch is its own verb.
		Value FsListKind(const std::vector<Value>& args, int line, BOOL bDirs)
		{
			String dir = ArgStr(args, 0, line, L"directory");
			String pattern = L"*";
			String spec;
			WIN32_FIND_DATAW fd;
			HANDLE hFind;
			Value v;

			if (args.size() > 1)
			{
				pattern = ArgStr(args, 1, line, L"pattern");
			}
			spec = dir;
			if (!spec.empty() && (spec[spec.size() - 1] != L'\\'))
			{
				spec += L'\\';
			}
			spec += pattern;

			v.kind = VK_LIST;
			v.list = ListPtr(new std::vector<Value>());
			hFind = FindFirstFileW(spec.c_str(), &fd);
			if (hFind != INVALID_HANDLE_VALUE)
			{
				do
				{
					bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

					if (isDir && !bDirs)
					{
						continue;      // a file listing leaves the directories out
					}
					if (!isDir && bDirs)
					{
						continue;      // and a directory listing leaves the files out
					}
					// The two names every directory has and nobody wants. The file
					// listing never had to skip them: they are directories.
					if (bDirs && (!_wcsicmp(fd.cFileName, L".") || !_wcsicmp(fd.cFileName, L"..")))
					{
						continue;
					}
					v.list->push_back(MakeStr(dir + ((!dir.empty() && (dir[dir.size() - 1] == L'\\')) ? L"" : L"\\") + fd.cFileName));
				}
				while (FindNextFileW(hFind, &fd));
				FindClose(hFind);
			}
			// FindFirstFile hands back directory order, which happens to be
			// sorted on NTFS but is not promised. The boot chain replaced a set
			// of racing `start` calls with a loop, so the loop has to be the
			// thing that decides the order: sort by name, case insensitive.
			std::sort(v.list->begin(), v.list->end(), [](const Value& a, const Value& b) {
				return _wcsicmp(a.s.c_str(), b.s.c_str()) < 0;
			});
			return v;
		}

		// Every file in a directory that matches a pattern. Directories are not
		// in here -- that is what fs.list_dirs is for, and this verb's answer has
		// never had a way to say "a directory was left out", which is why the
		// pair exists rather than one verb with a switch.
		Value HostFsList(Interp&, std::vector<Value>& args, int line)
		{
			return FsListKind(args, line, FALSE);
		}

		// Every subdirectory of a directory that matches a pattern, as absolute
		// paths, sorted the same way. "." and ".." are not subdirectories.
		Value HostFsListDirs(Interp&, std::vector<Value>& args, int line)
		{
			return FsListKind(args, line, TRUE);
		}

		Value HostFsDelete(Interp&, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"path");
			bool missingOk = OptBool(args, 1, L"missing_ok", false);
			HRESULT hr;

			if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
			{
				if (missingOk)
				{
					return MakeNil();
				}
				throw Error(ERROR_FILE_NOT_FOUND, L"fs.delete: no such file or directory: " + path, line);
			}
			hr = FsDelete(path.c_str());
			if (hr != S_OK)
			{
				TCHAR tzBuf[MAX_NAME];
				UStrPrint(tzBuf, TEXT("fs.delete failed (%u)"), (UINT) hr);
				throw Error(hr, tzBuf, line);
			}
			TraceVerb(String(L"fs.delete ") + path, line, S_OK);
			return MakeNil();
		}

		Value HostFsMkdir(Interp&, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"path");
			HRESULT hr = DirCreate(path.c_str());

			if (hr != S_OK)
			{
				TCHAR tzBuf[MAX_NAME];
				UStrPrint(tzBuf, TEXT("fs.mkdir failed (%u)"), (UINT) hr);
				throw Error(hr, tzBuf, line);
			}
			TraceVerb(String(L"fs.mkdir ") + path, line, S_OK);
			return MakeNil();
		}

		// File bytes to text: a BOM decides, otherwise valid UTF-8 is UTF-8 and
		// anything else is the ANSI code page. That is the rule the script
		// loader uses (src/lang/run.cpp), and it matters here for the same
		// reason -- one PE holds both kinds without a BOM, and guessing wrong
		// does not look broken, it produces different Chinese characters.
		String DecodeFileText(const std::vector<BYTE>& data)
		{
			// One rule for the whole program, shared with the script loader;
			// see DecodeTextBytes for why it is not two.
			return data.empty() ? String() : DecodeTextBytes(&data[0], data.size());
		}

		// How a text file's bytes are spelled, named as the option string a
		// script passes to fs.write.
		//
		// The default is plain UTF-8 with no byte-order mark. Writing a mark on
		// every create is not harmless: to cmd.exe the mark is not a mark, it is
		// three characters of garbage in front of the first line, which is how a
		// generated .cmd or .bat stops running. Anything that wants the mark
		// asks for utf8bom.
		enum TextEncoding { TE_UTF8 = 0, TE_UTF8BOM, TE_ANSI, TE_UTF16LE, TE_UTF16BE };

		bool EncodingFromName(const String& name, TextEncoding *out)
		{
			if (name == L"utf8")    { *out = TE_UTF8;     return true; }
			if (name == L"utf8bom") { *out = TE_UTF8BOM;  return true; }
			if (name == L"ansi")    { *out = TE_ANSI;     return true; }
			if (name == L"utf16")   { *out = TE_UTF16LE;  return true; }
			if (name == L"utf16be") { *out = TE_UTF16BE;  return true; }
			return false;
		}

		// Text to the bytes that encoding writes. withBom is false only when
		// appending: a second mark in the middle of a file is text, not a mark.
		std::vector<BYTE> EncodeText(const String& text, TextEncoding enc, bool withBom)
		{
			std::vector<BYTE> bytes;

			if ((enc == TE_UTF16LE) || (enc == TE_UTF16BE))
			{
				if (withBom)
				{
					bytes.push_back((enc == TE_UTF16LE) ? 0xFF : 0xFE);
					bytes.push_back((enc == TE_UTF16LE) ? 0xFE : 0xFF);
				}
				if (!text.empty())
				{
					size_t cb = text.size() * sizeof(wchar_t);
					size_t at = bytes.size();

					bytes.resize(at + cb);
					UMemCopy(&bytes[at], text.data(), (UINT) cb);
					if (enc == TE_UTF16BE)
					{
						for (size_t i = at; i < at + cb; i += 2)
						{
							BYTE b = bytes[i];

							bytes[i] = bytes[i + 1];
							bytes[i + 1] = b;
						}
					}
				}
				return bytes;
			}
			if ((enc == TE_UTF8BOM) && withBom)
			{
				bytes.push_back(0xEF);
				bytes.push_back(0xBB);
				bytes.push_back(0xBF);
			}

			{
				UINT uCodePage = (enc == TE_ANSI) ? CP_ACP : CP_UTF8;
				int cb = WideCharToMultiByte(uCodePage, 0, text.c_str(), (int) text.size(),
					NULL, 0, NULL, NULL);

				if (cb > 0)
				{
					size_t at = bytes.size();

					bytes.resize(at + (size_t) cb);
					WideCharToMultiByte(uCodePage, 0, text.c_str(), (int) text.size(),
						(LPSTR) &bytes[at], cb, NULL, NULL);
				}
			}
			return bytes;
		}

		// What a file's own bytes say about how it is spelled, so a rewrite can
		// put back what was there. A byte-order mark is a declaration and decides
		// by itself; with none the reader's own rule applies -- valid UTF-8 is
		// UTF-8, anything else is the local code page -- or an ANSI file would
		// come back as UTF-8 the first time one line of it was edited.
		TextEncoding DetectEncoding(const std::vector<BYTE>& data)
		{
			if ((data.size() >= 3) && (data[0] == 0xEF) && (data[1] == 0xBB) && (data[2] == 0xBF))
			{
				return TE_UTF8BOM;
			}
			if ((data.size() >= 2) && (data[0] == 0xFF) && (data[1] == 0xFE))
			{
				return TE_UTF16LE;
			}
			if ((data.size() >= 2) && (data[0] == 0xFE) && (data[1] == 0xFF))
			{
				return TE_UTF16BE;
			}
			if (!data.empty())
			{
				if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
						(LPCCH) &data[0], (int) data.size(), NULL, 0) > 0)
				{
					return TE_UTF8;
				}
			}
			return TE_ANSI;
		}

		HRESULT WriteTextFile(PCTSTR ptzPath, const String& text, BOOL bAppend, TextEncoding enc)
		{
			std::vector<BYTE> bytes = EncodeText(text, enc, !bAppend);

			return FsWriteAll(ptzPath, bytes.empty() ? NULL : &bytes[0], (DWORD) bytes.size(), bAppend);
		}

		Value HostFsRead(Interp&, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"path");
			std::vector<BYTE> data;
			HRESULT hr = FsReadAll(path.c_str(), &data);

			if (hr != S_OK)
			{
				throw Error(hr, String(L"fs.read: ") + path, line);
			}
			TraceVerb(String(L"fs.read ") + path, line, S_OK);
			return MakeStr(DecodeFileText(data));
		}

		// A trailing newline ends the last line rather than starting an empty
		// one, which is what every editor and every count expects.
		std::vector<String> SplitTextLines(const String& text)
		{
			std::vector<String> out;
			size_t at = 0;

			while (at < text.size())
			{
				size_t nl = text.find(L'\n', at);
				String lineText = (nl == String::npos) ? text.substr(at) : text.substr(at, nl - at);

				if (!lineText.empty() && (lineText[lineText.size() - 1] == L'\r'))
				{
					lineText.erase(lineText.size() - 1);
				}
				out.push_back(lineText);
				if (nl == String::npos)
				{
					break;
				}
				at = nl + 1;
			}
			return out;
		}


		Value HostFsWriteAppend(std::vector<Value>& args, int line, BOOL bAppend)
		{
			String path = ArgStr(args, 0, line, L"path");
			String text = (args.size() > 1) ? ToDisplay(args[1]) : String();
			TextEncoding enc = TE_UTF8;
			const Value *name = Field(args, 2, L"encoding");
			HRESULT hr;

			if (name && ((name->kind != VK_STR) || (!EncodingFromName(name->s, &enc))))
			{
				throw Error(0x80070057,
					L"fs.write: encoding must be one of \"utf8\", \"utf8bom\", \"ansi\", \"utf16\"",
					line);
			}
			hr = WriteTextFile(path.c_str(), text, bAppend, enc);

			if (hr != S_OK)
			{
				throw Error(hr, String(bAppend ? L"fs.append: " : L"fs.write: ") + path, line);
			}
			TraceVerb(String(bAppend ? L"fs.append " : L"fs.write ") + path, line, S_OK);
			return MakeNil();
		}

		Value HostFsWrite(Interp&, std::vector<Value>& args, int line)
		{
			return HostFsWriteAppend(args, line, FALSE);
		}

		Value HostFsAppend(Interp&, std::vector<Value>& args, int line)
		{
			return HostFsWriteAppend(args, line, TRUE);
		}

		Value HostFsRename(Interp&, std::vector<Value>& args, int line)
		{
			String from = ArgStr(args, 0, line, L"from");
			String to = ArgStr(args, 1, line, L"to");
			HRESULT hr = FsRename(from.c_str(), to.c_str());

			if (hr != S_OK)
			{
				throw Error(hr, String(L"fs.rename: ") + from + L" -> " + to, line);
			}
			TraceVerb(String(L"fs.rename ") + from, line, S_OK);
			return MakeNil();
		}

		Value HostFsHardlink(Interp&, std::vector<Value>& args, int line)
		{
			String lnk = ArgStr(args, 0, line, L"link");
			String target = ArgStr(args, 1, line, L"target");
			HRESULT hr = FsHardLink(lnk.c_str(), target.c_str());

			if (hr != S_OK)
			{
				throw Error(hr, String(L"fs.hardlink: ") + lnk + L" -> " + target, line);
			}
			TraceVerb(String(L"fs.hardlink ") + lnk, line, S_OK);
			return MakeNil();
		}

		Value HostFsTempDir(Interp&, std::vector<Value>&, int line)
		{
			TCHAR tzBuf[MAX_PATH];
			HRESULT hr = FsTempDir(tzBuf, _NumOf(tzBuf));

			if (hr != S_OK)
			{
				throw Error(hr, L"fs.temp_dir could not be read", line);
			}
			TraceVerb(String(L"fs.temp_dir ") + tzBuf, line, S_OK);
			return MakeStr(tzBuf);
		}

		Value HostFsChdir(Interp&, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"path");

			// A directory that cannot be entered is not an error: the reference
			// reports success there, on the reasoning that failing a boot script
			// over a folder it cannot enter is worse than carrying on. The trace
			// says which of the two happened.
			if (!SetCurrentDirectoryW(path.c_str()))
			{
				TraceVerb(String(L"fs.chdir ") + path + L" (not entered)", line, S_OK);
				return MakeNil();
			}
			TraceVerb(String(L"fs.chdir ") + path, line, S_OK);
			return MakeNil();
		}

		Value HostFsCleanTemp(Interp&, std::vector<Value>&, int line)
		{
			HRESULT hr = CleanTempFolder();

			if (hr != S_OK)
			{
				throw Error(hr, L"fs.clean_temp: the temp folder could not be emptied", line);
			}
			TraceVerb(L"fs.clean_temp", line, S_OK);
			return MakeNil();
		}

		// Line editing on a text file, 1-based. Without an option the named line
		// is replaced; {insert: true} puts the text before it, {delete: true}
		// removes it. Writing it back is UTF-8 with a BOM, the same as fs.write,
		// so what this produces reads back the way everything else here does.
		Value HostFsEditLine(Interp&, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"path");
			long long n = ArgInt(args, 1, line, L"line number");
			String text = (args.size() > 2) ? ToDisplay(args[2]) : String();
			bool insert = OptBool(args, 3, L"insert", false);
			bool remove = OptBool(args, 3, L"delete", false);
			std::vector<BYTE> data;
			std::vector<String> lines;
			String whole;
			HRESULT hr;

			if (n < 1)
			{
				throw Error(0x80070057, L"fs.edit_line: line numbers start at 1", line);
			}
			hr = FsReadAll(path.c_str(), &data);
			if (hr != S_OK)
			{
				throw Error(hr, String(L"fs.edit_line: ") + path, line);
			}
			// What the file already is, so editing one line does not quietly
			// re-spell the whole file.
			TextEncoding enc = DetectEncoding(data);

			lines = SplitTextLines(DecodeFileText(data));
			{
				size_t at = (size_t) (n - 1);

				if (remove)
				{
					if (at < lines.size())
					{
						lines.erase(lines.begin() + (long) at);
					}
				}
				else if (insert)
				{
					if (at > lines.size())
					{
						at = lines.size();
					}
					lines.insert(lines.begin() + (long) at, text);
				}
				else
				{
					if (at >= lines.size())
					{
						throw Error(0x80070057, String(L"fs.edit_line: there is no line ")
							+ ToText(MakeInt(n)) + L" in " + path, line);
					}
					lines[at] = text;
				}
			}
			for (size_t i = 0; i < lines.size(); i++)
			{
				whole += lines[i];
				whole += L"\r\n";
			}
			hr = WriteTextFile(path.c_str(), whole, FALSE, enc);
			if (hr != S_OK)
			{
				throw Error(hr, String(L"fs.edit_line could not write ") + path, line);
			}
			TraceVerb(String(L"fs.edit_line ") + path, line, S_OK);
			return MakeNil();
		}

		// Attributes have no legacy command; walk it here. `recurse` mirrors the
		// "attrib +s X:\Users\*.* /D /S" line in the shipped Computer.bat.
		// Every file has to take the change for the call to have succeeded: "I
		// set the attributes on that tree" is a claim about all of it. A tree that
		// cannot be finished is walked to the end anyway -- as much as possible is
		// still better than stopping at the first refusal -- and the code that
		// stopped it comes back through *pdwError.
		bool SetAttrTree(const String& path, DWORD add, DWORD remove, bool recurse,
			DWORD *pdwError)
		{
			DWORD attr = GetFileAttributesW(path.c_str());
			bool bOk = true;

			if (attr == INVALID_FILE_ATTRIBUTES)
			{
				if (*pdwError == ERROR_SUCCESS)
				{
					*pdwError = GetLastError();
				}
				return false;
			}
			if (!SetFileAttributesW(path.c_str(), (attr | add) & ~remove))
			{
				if (*pdwError == ERROR_SUCCESS)
				{
					*pdwError = GetLastError();
				}
				bOk = false;
			}

			// A reparse point -- a junction, a mount point -- is not descended
			// into: it can point back at a directory already being walked, and the
			// walk would then never end. The reparse point's own attributes are
			// still set here, which is what "hide this folder" means.
			if (recurse && (attr & FILE_ATTRIBUTE_DIRECTORY) &&
				(!(attr & FILE_ATTRIBUTE_REPARSE_POINT)))
			{
				WIN32_FIND_DATAW fd;
				String spec = path + L"\\*";
				HANDLE hFind = FindFirstFileW(spec.c_str(), &fd);

				if (hFind != INVALID_HANDLE_VALUE)
				{
					do
					{
						if ((wcscmp(fd.cFileName, L".") == 0) || (wcscmp(fd.cFileName, L"..") == 0))
						{
							continue;
						}
						if (!SetAttrTree(path + L"\\" + fd.cFileName, add, remove, true,
								pdwError))
						{
							bOk = false;
						}
					}
					while (FindNextFileW(hFind, &fd));
					FindClose(hFind);
				}
				else
				{
					// An empty folder answers ERROR_FILE_NOT_FOUND -- nothing to
					// walk, and nothing missing. Anything else means the walk could
					// not be made at all, and "every file in that tree took the
					// change" is then a claim this call cannot support. Same shape
					// as PathSizeBytes.
					DWORD dwErr = GetLastError();

					if (dwErr != ERROR_FILE_NOT_FOUND)
					{
						if (*pdwError == ERROR_SUCCESS)
						{
							*pdwError = dwErr;
						}
						bOk = false;
					}
				}
			}
			return bOk;
		}

		// One attribute, in the three states the record can express: not named
		// means "leave it alone", true adds it and false takes it away. A field
		// that is there but is not a bool is a mistake worth naming -- reading it
		// as false would clear an attribute nobody asked about.
		void SetOneAttr(const std::vector<Value>& args, size_t i, const wchar_t *key,
			DWORD attr, DWORD *padd, DWORD *premove, int line)
		{
			const Value *v = Field(args, i, key);

			if (!v)
			{
				return;
			}
			if (v->kind != VK_BOOL)
			{
				throw Error(0x80070057,
					String(L"fs.set_attr: ") + key + L" must be true or false", line);
			}
			if (v->b)
			{
				*padd |= attr;
			}
			else
			{
				*premove |= attr;
			}
		}

		// fs.set_attr(path, {hidden: bool, system: bool, readonly: bool,
		//                    recurse: bool})
		//
		// The first version had a second set of "clear_*" options for the false
		// half, which is two spellings for one decision -- and it left "leave this
		// attribute alone" and "take it off" looking the same in a call that names
		// neither.
		Value HostFsSetAttr(Interp&, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"path");
			DWORD add = 0;
			DWORD remove = 0;
			bool recurse = OptBool(args, 1, L"recurse", false);

			SetOneAttr(args, 1, L"system", FILE_ATTRIBUTE_SYSTEM, &add, &remove, line);
			SetOneAttr(args, 1, L"hidden", FILE_ATTRIBUTE_HIDDEN, &add, &remove, line);
			SetOneAttr(args, 1, L"readonly", FILE_ATTRIBUTE_READONLY, &add, &remove, line);

			// Existence is checked on the path the script named, so the message
			// below can name the right thing. A child that disappears mid-walk is
			// a different claim and gets the other message.
			if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
			{
				throw Error(ERROR_FILE_NOT_FOUND, L"fs.set_attr: no such path: " + path, line);
			}

			DWORD dwError = ERROR_SUCCESS;

			if (!SetAttrTree(path, add, remove, recurse, &dwError))
			{
				// The named path is there; something inside could not be changed
				// (a read-only volume, a file held open). That is not the same
				// failure, and it does not get the same message.
				throw Error((HRESULT) dwError,
					String(L"fs.set_attr: the attributes of ") + path
						+ L" could not all be set",
					line);
			}
			TraceVerb(L"fs.set_attr", line, S_OK);
			return MakeNil();
		}

		// fs.find_on_drives(relative) -- every drive root where that relative
		// path exists, as a list of full paths.
		//
		// This is what replaces the "LOAD \relative" search that used to run from
		// whichever drive answered first. Searching and deciding are separate
		// here: this only lists, the script names the one it wants, and `run` still
		// takes one concrete path -- so nothing is ever executed from a drive the
		// script did not name.
		Value HostFsFindOnDrives(Interp&, std::vector<Value>& args, int line)
		{
			String rel = ArgStr(args, 0, line, L"relative path");
			TCHAR tzRoots[MAX_PATH];
			Value out;

			out.kind = VK_LIST;
			out.list = ListPtr(new std::vector<Value>());

			// A leading separator is the shape that was removed. Accepting it
			// would hand back one hit per drive, which is that search again with
			// extra steps.
			if (!rel.empty() && ((rel[0] == L'\\') || (rel[0] == L'/')))
			{
				throw Error(0x80070057,
					L"fs.find_on_drives: the path must be relative, with no leading separator",
					line);
			}
			if (!GetLogicalDriveStrings(_NumOf(tzRoots), tzRoots))
			{
				return out;
			}
			for (PTSTR ptzRoot = tzRoots; *ptzRoot; ptzRoot += UStrLen(ptzRoot) + 1)
			{
				String full = String(ptzRoot) + rel;

				if (UFileExist((PTSTR) full.c_str()))
				{
					out.list->push_back(MakeStr(full));
				}
			}
			TraceVerb(String(L"fs.find_on_drives ") + rel, line, S_OK);
			return out;
		}

		// fs.stat(path) -- what the filesystem says about a path.
		//
		// nil when there is nothing there, so "is it there and what is it" is one
		// call rather than three. A directory is sized as everything under it,
		// which is what the SIZE it replaces meant and what "how big is this
		// folder" has to mean to be useful; is_dir says which reading applies.
		Value HostFsStat(Interp&, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"path");
			WIN32_FILE_ATTRIBUTE_DATA fa;
			Value v;

			if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa))
			{
				TraceVerb(String(L"fs.stat ") + path + L" (absent)", line, S_OK);
				return MakeNil();
			}

			v.kind = VK_RECORD;
			v.rec = RecordPtr(new std::vector<std::pair<String, Value> >());
			{
				UINT64 uSize = 0;

				if (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				{
					DWORD dwError = ERROR_SUCCESS;

					if (!PathSizeBytes(path.c_str(), &uSize, &dwError))
					{
						// A tree whose size cannot be established has no size to
						// report -- a name too long for the walk, or a folder this
						// caller may not list. Zero is a size, and it is not this
						// one.
						throw Error((HRESULT) dwError,
							String(L"fs.stat: the size of ") + path + L" could not be measured", line);
					}
				}
				else
				{
					uSize = ((UINT64) fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
				}

				RecPut(v, L"size",     MakeInt((long long) uSize));
				RecPut(v, L"is_dir",   MakeBool((fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0));
				RecPut(v, L"hidden",   MakeBool((fa.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0));
				RecPut(v, L"system",   MakeBool((fa.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) != 0));
				RecPut(v, L"readonly", MakeBool((fa.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0));
				RecPut(v, L"modified", MakeInt(FileTimeToEpoch(fa.ftLastWriteTime)));
			}
			TraceVerb(String(L"fs.stat ") + path, line, S_OK);
			return v;
		}

		// fs.hash(path, {algo}) -- the digest of a file, lowercase hex.
		//
		// Read in chunks rather than "read the file and hash the buffer": the file
		// may be a WIM or an install image, and the whole point of the verb is to
		// work on one. That is also the difference from the HASH it replaces,
		// which read the whole thing into memory and answered success even when
		// the read had failed.
		Value HostFsHash(Interp&, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"path");
			String algo = OptStr(args, 1, L"algo");
			ALG_ID id;
			DWORD dwProvType;
			HCRYPTPROV hProv = 0;
			HCRYPTHASH hHash = 0;
			HANDLE hFile = INVALID_HANDLE_VALUE;
			String out;

			if (algo.empty())
			{
				algo = L"sha256";
			}
			if (algo == L"sha256")
			{
#ifndef CALG_SHA_256
#define CALG_SHA_256 0x0000800c
#endif
				id = CALG_SHA_256;
				// The base provider has no SHA-256 -- asking it answers
				// NTE_BAD_ALGID, which is the quirk the legacy HASH still
				// reproduces. This verb is new, so it is not bound by it: the AES
				// provider is where SHA-256 lives, and it also has the other two.
				dwProvType = PROV_RSA_AES;
			}
			else if (algo == L"sha1")
			{
				id = CALG_SHA1;
				dwProvType = PROV_RSA_FULL;
			}
			else if (algo == L"md5")
			{
				id = CALG_MD5;
				dwProvType = PROV_RSA_FULL;
			}
			else
			{
				throw Error(0x80070057,
					L"fs.hash: algo must be one of \"sha256\", \"sha1\", \"md5\"", line);
			}

			hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			if (hFile == INVALID_HANDLE_VALUE)
			{
				throw Error((HRESULT) GetLastError(), String(L"fs.hash: ") + path, line);
			}
			if (!CryptAcquireContext(&hProv, NULL, NULL, dwProvType, CRYPT_VERIFYCONTEXT) ||
				!CryptCreateHash(hProv, id, 0, 0, &hHash))
			{
				HRESULT hr = (HRESULT) GetLastError();

				CloseHandle(hFile);
				if (hProv)
				{
					CryptReleaseContext(hProv, 0);
				}
				throw Error(hr, L"fs.hash: the crypto provider would not start a digest", line);
			}
			{
				std::vector<BYTE> chunk(64 * 1024);
				bool ok = true;

				for (;;)
				{
					DWORD dwRead = 0;

					if (!ReadFile(hFile, &chunk[0], (DWORD) chunk.size(), &dwRead, NULL))
					{
						ok = false;
						break;
					}
					if (dwRead == 0)
					{
						break;		// end of file
					}
					// A read that fails half way through is a failed digest, not a
					// digest of the part that arrived.
					if (!CryptHashData(hHash, &chunk[0], dwRead, 0))
					{
						ok = false;
						break;
					}
				}
				if (!ok)
				{
					HRESULT hr = (HRESULT) GetLastError();

					CloseHandle(hFile);
					CryptDestroyHash(hHash);
					CryptReleaseContext(hProv, 0);
					throw Error(hr, String(L"fs.hash: ") + path, line);
				}
			}
			{
				BYTE abDigest[64];
				DWORD cb = _NumOf(abDigest);
				static const wchar_t c_hex[] = L"0123456789abcdef";

				if (!CryptGetHashParam(hHash, HP_HASHVAL, abDigest, &cb, 0))
				{
					HRESULT hr = (HRESULT) GetLastError();

					CloseHandle(hFile);
					CryptDestroyHash(hHash);
					CryptReleaseContext(hProv, 0);
					throw Error(hr, String(L"fs.hash: ") + path, line);
				}
				for (DWORD i = 0; i < cb; i++)
				{
					out += c_hex[(abDigest[i] >> 4) & 0xF];
					out += c_hex[abDigest[i] & 0xF];
				}
			}
			CloseHandle(hFile);
			CryptDestroyHash(hHash);
			CryptReleaseContext(hProv, 0);

			TraceVerb(String(L"fs.hash ") + algo + L" " + path, line, S_OK);
			return MakeStr(out);
		}

		// --- shell ----------------------------------------------------------------

		Value HostShellShortcut(Interp&, std::vector<Value>& args, int line)
		{
			String lnk = ArgStr(args, 0, line, L"shortcut path");
			String target = ArgStr(args, 1, line, L"target");
			String extra = OptStr(args, 2, L"params");
			String icon = OptStr(args, 2, L"icon");
			long long iconIndex = 0;
			HRESULT hr;

			// The icon index is its own argument, not a "#N" glued onto the
			// file name. Gluing it on was the shipped config's habit, and it
			// produced no icon at all: the whole "shell32.dll#238" string went
			// to SetIconLocation as if it were a file path.
			{
				const Value *idx = Field(args, 2, L"icon_index");

				if (idx && (idx->kind == VK_INT))
				{
					iconIndex = idx->i;
				}
			}
			hr = CreateShortcut(lnk.c_str(), target.c_str(), extra.c_str(),
				icon.empty() ? NULL : icon.c_str(), (INT) iconIndex, FALSE, FALSE);
			if (hr != S_OK)
			{
				TCHAR tzBuf[MAX_STR];
				UStrPrint(tzBuf, TEXT("shell.shortcut failed (0x%08X)"), (UINT) hr);
				throw Error(hr, tzBuf, line);
			}
			TraceVerb(String(L"shell.shortcut ") + lnk, line, S_OK);
			return MakeNil();
		}


		Value HostShellStart(Interp&, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"shell path");
			bool system = OptBool(args, 1, L"system", false);
			HRESULT hr;

			// { system: true } used to become the SHEL "-sys" switch, which the
			// legacy parser recognises and then skips. It stays in the trace so the
			// intent is visible, but nothing pretends to act on it.
			hr = SetSystemShell(path.c_str());
			if (hr != S_OK)
			{
				throw Error(hr, String(L"shell.start could not register ") + path, line);
			}
			TraceVerb(String(system ? L"shell.start -sys " : L"shell.start ") + path, line, S_OK);
			return MakeNil();
		}

		// Both of these are WinXShell's, kept because the language has no UI
		// layer yet. They disappear when the ui module lands; until then the boot
		// chain stays single language and the trail still shows exactly one
		// WinXShell call per action.
		Value HostUiPinToTaskbar(Interp&, std::vector<Value>& args, int line)
		{
			String name = ArgStr(args, 0, line, L"program name");
			String exe = OptStr(args, 1, L"winxshell");
			ProcSpec spec;

			if (exe.empty())
			{
				exe = EnvValue(L"ProgramFiles") + L"\\WinXShell\\WinXShell.exe";
			}
			spec.cmd = QuoteAlways(exe) + L" -code " +
				QuoteAlways(L"PinToTaskbar(" + LuaQuote(name, line, L"program name") + L")");
			spec.hidden = true;
			SpawnAndTrace(L"ui.pin_to_taskbar", spec, false, line, NULL, NULL);
			return MakeNil();
		}

		Value HostUiFolderOption(Interp&, std::vector<Value>& args, int line)
		{
			String name = ArgStr(args, 0, line, L"option");
			String value = L"1";
			String exe = EnvValue(L"ProgramFiles") + L"\\WinXShell\\WinXShell.exe";
			ProcSpec spec;

			// The value goes into the Lua call as a literal, so only a literal is
			// accepted. Text here would be a fragment of Lua to run, which is the
			// whole of the hole this closes.
			if (args.size() > 1)
			{
				if (args[1].kind == VK_BOOL)
				{
					value = args[1].b ? L"true" : L"false";
				}
				else if (args[1].kind == VK_INT)
				{
					value = ToText(args[1]);
				}
				else
				{
					throw Error(0x80070057,
						L"ui.folder_option: the value must be true, false or a number",
						line);
				}
			}
			spec.cmd = QuoteAlways(exe) + L" -code " +
				QuoteAlways(L"FolderOptions:Set(" + LuaQuote(name, line, L"option")
					+ L", " + value + L")");
			spec.hidden = true;
			SpawnAndTrace(L"ui.folder_option", spec, false, line, NULL, NULL);
			return MakeNil();
		}

		Value HostUiWallpaper(Interp&, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"path");
			HRESULT hr = SetWallpaper(path.c_str());

			if (hr != S_OK)
			{
				throw Error(ERROR_FILE_NOT_FOUND, L"ui.wallpaper: no such image: " + path, line);
			}
			TraceVerb(String(L"ui.wallpaper ") + path, line, S_OK);
			return MakeNil();
		}

		// --- hotkey ---------------------------------------------------------------

		// ui.hotkey(vk, closure[, {mods}]) -- run a closure when a key is
		// pressed.
		//
		// The binding is kept, not written down. It used to be a command string
		// parked in the registry and parsed again by whichever process owned the
		// window -- text out of a script, handed to a parser, on the other side
		// of a process boundary. That is gone: the chain runs in the process that
		// owns the window, so the binding stays a value.
		//
		// The interpreter goes with it, because the closure's captured scope has
		// to outlive the script that made it. Nothing else in this file has to
		// know where the interpreter lives.
		Value HostUiHotkey(Interp& in, std::vector<Value>& args, int line)
		{
			long long vk = ArgInt(args, 0, line, L"virtual key");
			const Value *mods = Field(args, 2, L"mods");
			UINT uMods = 0;
			std::shared_ptr<Interp> owner = in.self.lock();

			if ((args.size() < 2) || (args[1].kind != VK_FUNC) || !args[1].fn)
			{
				throw Error(0x80070057,
					L"ui.hotkey takes a key and a closure, e.g. ui.hotkey(112, fn() { ... })",
					line);
			}
			// RegisterHotKey takes the key as a number: 0 is not a key, and above
			// 254 is not in the table it can fire.
			if ((vk < 1) || (vk > 254))
			{
				throw Error(0x80070057, L"ui.hotkey: the key must be between 1 and 254", line);
			}
			// Nothing calls it with arguments, so a closure that wants them would
			// be reading parameters that are always nil. Refused here rather than
			// discovered when the key is pressed.
			if (!args[1].fn->params.empty())
			{
				throw Error(0x80070057,
					L"ui.hotkey: the closure takes no parameters (nothing passes it any)", line);
			}
			if (mods && (mods->kind == VK_INT))
			{
				uMods = (UINT) mods->i;
			}
			if (!owner || !AddHotkeyBinding((UINT) vk, uMods, line, args[1].fn, owner))
			{
				throw Error(0x80070057, L"ui.hotkey: there are only eight slots", line);
			}
			TraceVerb(String(L"ui.hotkey ") + ToText(MakeInt(vk)), line, S_OK);
			return MakeNil();
		}

		// --- ui -------------------------------------------------------------------

		Value HostUiSplash(Interp&, std::vector<Value>& args, int line)
		{
			// ui.splash(img) shows that image; ui.splash() with no image -- which
			// is what ui.splash_clear() maps to -- takes the splash down.
			if ((args.size() > 0) && (args[0].kind == VK_STR))
			{
				HRESULT hr = ShowLogo(args[0].s.c_str());

				if (hr != S_OK)
				{
					throw Error(hr, String(L"ui.splash failed for ") + args[0].s, line);
				}
				TraceVerb(String(L"ui.splash ") + args[0].s, line, S_OK);
			}
			else
			{
				ShowLogo(NULL);
				TraceVerb(L"ui.splash_clear", line, S_OK);
			}
			return MakeNil();
		}


		Value HostUiSplashText(Interp&, std::vector<Value>& args, int line)
		{
			String text = ArgStr(args, 0, line, L"text");
			const Value *color = Field(args, 1, L"color");
			const Value *x = Field(args, 1, L"x");
			const Value *y = Field(args, 1, L"y");
			const Value *size = Field(args, 1, L"size");
			DWORD dwColor = 0;
			INT iLeft = -1, iTop = -1, iSize = -1;
			HRESULT hr;

			if (color && (color->kind == VK_INT)) { dwColor = (DWORD) color->i; }
			if (x && (x->kind == VK_INT))         { iLeft = (INT) x->i; }
			if (y && (y->kind == VK_INT))         { iTop = (INT) y->i; }
			if (size && (size->kind == VK_INT))   { iSize = (INT) size->i; }

			// -1 for right/bottom means "leave the box as it was", which is what
			// the legacy markers did by not naming them at all.
			hr = SetLogoText(text.c_str(), dwColor, iLeft, iTop, -1, -1, iSize, FALSE);
			if (hr != S_OK)
			{
				throw Error(hr, String(L"ui.splash_text failed: ") + text, line);
			}
			TraceVerb(String(L"ui.splash_text ") + text, line, S_OK);
			return MakeNil();
		}

		// --- native / drv / svc ---------------------------------------------------

		Value HostNativeCall(Interp&, std::vector<Value>& args, int line)
		{
			String dll;
			String proc;
			std::vector<String> strings;
			std::vector<PVOID> pv;
			LONG lResult = 0;
			HRESULT hr;

			if (args.size() < 2)
			{
				throw Error(0x80070057, L"native.call takes a library and a function name", line);
			}
			dll = ArgStr(args, 0, line, L"library");
			proc = ArgStr(args, 1, line, L"function");
			if (args.size() - 2 > 4)
			{
				throw Error(0x80070057, L"native.call takes at most four arguments", line);
			}
			// The strings are built first on purpose: pushing onto a vector can
			// move its storage, which would leave the pointers handed to the
			// callee dangling.
			for (size_t i = 2; i < args.size(); i++)
			{
				if (args[i].kind != VK_INT)
				{
					strings.push_back(ToDisplay(args[i]));
				}
			}
			{
				size_t k = 0;

				for (size_t i = 2; i < args.size(); i++)
				{
					if (args[i].kind == VK_INT)
					{
						pv.push_back((PVOID)(INT_PTR) args[i].i);
					}
					else
					{
						pv.push_back((PVOID) strings[k++].c_str());
					}
				}
			}

			hr = NativeCall(dll.c_str(), proc.c_str(), pv.empty() ? NULL : pv.data(),
				(UINT) pv.size(), &lResult);
			if (hr != S_OK)
			{
				TCHAR tzBuf[MAX_STR];
				UStrPrint(tzBuf, TEXT("%s,%s could not be called (%u)"), dll.c_str(), proc.c_str(), (UINT) hr);
				throw Error(hr, tzBuf, line);
			}
			// The callee's own return value is a value, not a status: EnableEUDC
			// answering 1 is the answer. The legacy command had to spell that
			// with the "positive codes are not failures" rule; here it is just
			// what the call returns.
			TraceVerb(String(L"native.call ") + dll + L"," + proc, line, S_OK);
			return MakeInt((long long) lResult);
		}

		Value HostNativeLoad(Interp&, std::vector<Value>& args, int line)
		{
			// The legacy "CALL $dll" with no comma calls DllRegisterServer, which
			// is what the shipped Initapp config relies on for FastEx64.dll.
			//
			// A library that will not load is not raised: the reference answers
			// success there, and a boot step should not stop the chain because an
			// optional DLL is absent.
			String dll = ArgStr(args, 0, line, L"library");
			LONG lResult = 0;
			HRESULT hr = NativeCall(dll.c_str(), NULL, NULL, 0, &lResult);

			if (hr == (HRESULT) ERROR_MOD_NOT_FOUND)
			{
				TraceVerb(String(L"native.load ") + dll + L" (not found)", line, S_OK);
				return MakeNil();
			}
			if (hr != S_OK)
			{
				TCHAR tzBuf[MAX_STR];
				UStrPrint(tzBuf, TEXT("%s: DllRegisterServer could not be called (%u)"), dll.c_str(), (UINT) hr);
				throw Error(hr, tzBuf, line);
			}
			if (FAILED((HRESULT) lResult))
			{
				TCHAR tzBuf[MAX_STR];
				UStrPrint(tzBuf, TEXT("%s: DllRegisterServer failed (0x%08X)"), dll.c_str(), (UINT) lResult);
				throw Error((HRESULT) lResult, tzBuf, line);
			}
			TraceVerb(String(L"native.load ") + dll, line, S_OK);
			return MakeNil();
		}

		// wait defaults to true: one inf at a time, which is the form that was
		// verified. wait: false hands back a process value instead, so several
		// infs can be loaded at once and awaited together - the same split
		// proc.run / proc.spawn has, for the same reason.
		Value HostDrvLoad(Interp& in, std::vector<Value>& args, int line)
		{
			String inf = ArgStr(args, 0, line, L"inf path");
			bool wait = OptBool(args, 1, L"wait", true);
			ProcSpec spec;
			HANDLE hProcess = NULL;
			DWORD dwPid = 0;
			int iExit = 0;

			spec.cmd = QuoteAlways(L"drvload.exe") + L" " + Quote(inf);
			spec.hidden = true;
			// The waiting form takes {timeout} exactly as proc.run does, and
			// hands drvload's own exit code back: a driver that did not install
			// is then something the caller can act on, and a drvload that never
			// returns no longer stops the boot chain for ever.
			SpawnAndTrace(L"drv.load", spec, wait, line,
				wait ? NULL : &hProcess, wait ? NULL : &dwPid,
				wait ? ProcTimeout(args, 1) : INFINITE, wait ? &iExit : NULL);
			if (wait)
			{
				return MakeInt((long long) iExit);
			}
			if (in.procs.size() < MAXIMUM_WAIT_OBJECTS)
			{
				in.procs[(long long) dwPid] = hProcess;
			}
			else
			{
				if (hProcess)
				{
					CloseHandle(hProcess);
				}
			}
			return MakeProcValue(dwPid);
		}


		Value HostSvcStart(Interp&, std::vector<Value>& args, int line)
		{
			String name = ArgStr(args, 0, line, L"service name");
			// "net start", which this replaced, returns once the service is
			// RUNNING; the reference only does that with -wait. Default false
			// so a caller can still ask for fire-and-forget.
			bool wait = OptBool(args, 1, L"wait", false);
			HRESULT hr = SetServiceState(name.c_str(), FALSE, wait ? TRUE : FALSE);

			// Already running is the state the caller asked for, so it is not an
			// error here. The reference's SERV reports 1056 in that case and the
			// legacy command keeps doing exactly that, but this verb is a
			// postcondition, not a report.
			if ((hr != S_OK) && (hr != (HRESULT) ERROR_SERVICE_ALREADY_RUNNING))
			{
				throw Error(hr, String(L"svc.start could not start ") + name, line);
			}
			TraceVerb(String(L"svc.start ") + name + (wait ? L" (wait)" : L""), line, S_OK);
			return MakeNil();
		}

		Value HostSvcStop(Interp&, std::vector<Value>& args, int line)
		{
			String name = ArgStr(args, 0, line, L"service name");
			bool wait = OptBool(args, 1, L"wait", false);
			HRESULT hr = SetServiceState(name.c_str(), TRUE, wait ? TRUE : FALSE);

			// Already stopped is the state the caller asked for -- the same
			// postcondition rule svc.start uses for "already running".
			if ((hr != S_OK) && (hr != (HRESULT) ERROR_SERVICE_NOT_ACTIVE))
			{
				throw Error(hr, String(L"svc.stop could not stop ") + name, line);
			}
			TraceVerb(String(L"svc.stop ") + name + (wait ? L" (wait)" : L""), line, S_OK);
			return MakeNil();
		}

		Value HostSvcQuery(Interp&, std::vector<Value>& args, int line)
		{
			String name = ArgStr(args, 0, line, L"service name");
			SERVICE_STATUS sStatus = {0};
			HRESULT hr = QueryServiceState(name.c_str(), &sStatus);
			Value out;

			// "Not installed" is an answer to a query, not a failure of one: a
			// query that finds nothing returns nil, so a script can ask whether a
			// service is there without wrapping the question in try. Every other
			// result still raises -- a service manager that will not answer is not
			// the same thing as a service that is not there.
			if (hr == (HRESULT) ERROR_SERVICE_DOES_NOT_EXIST)
			{
				TraceVerb(String(L"svc.query ") + name + L" (absent)", line, S_OK);
				return MakeNil();
			}
			if (hr != S_OK)
			{
				throw Error(hr, String(L"svc.query could not read ") + name, line);
			}
			// Everything the service manager reports that a caller can act on.
			// The state is the SERVICE_* number the platform uses (4 is RUNNING,
			// 1 is STOPPED) rather than a word, because that is what a script
			// compares against.
			out.kind = VK_RECORD;
			out.rec = RecordPtr(new std::vector<std::pair<String, Value> >());
			out.rec->push_back(std::make_pair(String(L"state"), MakeInt((long long) sStatus.dwCurrentState)));
			out.rec->push_back(std::make_pair(String(L"exit_code"), MakeInt((long long) sStatus.dwWin32ExitCode)));
			out.rec->push_back(std::make_pair(String(L"controls"), MakeInt((long long) sStatus.dwControlsAccepted)));
			out.rec->push_back(std::make_pair(String(L"checkpoint"), MakeInt((long long) sStatus.dwCheckPoint)));
			out.rec->push_back(std::make_pair(String(L"wait_hint"), MakeInt((long long) sStatus.dwWaitHint)));
			TraceVerb(String(L"svc.query ") + name, line, S_OK);
			return out;
		}

		Value HostSvcCreate(Interp&, std::vector<Value>& args, int line)
		{
			String name = ArgStr(args, 0, line, L"service name");
			String path = ArgStr(args, 1, line, L"binary path");
			String deps = OptStr(args, 2, L"deps");
			String display = OptStr(args, 2, L"display");
			DWORD dwType = SERVICE_WIN32_OWN_PROCESS;
			DWORD dwStart = SERVICE_DEMAND_START;
			DWORD dwError = SERVICE_ERROR_NORMAL;
			HRESULT hr;
			const Value *v;
			// lpDependencies is a MULTI_SZ: a double-NUL terminated list, even
			// when it holds a single name. Passing deps.c_str() -- one NUL --
			// let CreateService walk off the end of the string looking for the
			// second one, so the dependency list came from whatever happened to
			// be next on the heap.
			std::vector<TCHAR> vDeps;
			PCTSTR pDeps;

			if (!deps.empty())
			{
				vDeps.assign(deps.begin(), deps.end());
				vDeps.push_back(0);
				vDeps.push_back(0);
				pDeps = vDeps.data();
			}
			else
			{
				pDeps = NULL;
			}

			v = Field(args, 2, L"type");
			if (v && (v->kind == VK_INT)) { dwType = (DWORD) v->i; }
			v = Field(args, 2, L"start");
			if (v && (v->kind == VK_INT)) { dwStart = (DWORD) v->i; }
			v = Field(args, 2, L"error");
			if (v && (v->kind == VK_INT)) { dwError = (DWORD) v->i; }

			hr = CreateServiceEntry(name.c_str(), path.c_str(),
				display.empty() ? NULL : display.c_str(),
				dwType, dwStart, dwError, pDeps);
			if (hr != S_OK)
			{
				throw Error(hr, String(L"svc.create could not create ") + name, line);
			}
			TraceVerb(String(L"svc.create ") + name, line, S_OK);
			return MakeNil();
		}

		Value HostSvcDelete(Interp&, std::vector<Value>& args, int line)
		{
			String name = ArgStr(args, 0, line, L"service name");
			bool stop = OptBool(args, 1, L"stop", false);
			HRESULT hr = DeleteServiceEntry(name.c_str(), stop ? TRUE : FALSE);

			if (hr != S_OK)
			{
				throw Error(hr, String(L"svc.delete could not delete ") + name, line);
			}
			TraceVerb(String(L"svc.delete ") + name + (stop ? L" (stop)" : L""), line, S_OK);
			return MakeNil();
		}

		// --- env ------------------------------------------------------------------

		Value HostEnvGet(Interp&, std::vector<Value>& args, int line)
		{
			String name = ArgStr(args, 0, line, L"name");
			DWORD uLen = GetEnvironmentVariableW(name.c_str(), NULL, 0);

			if (uLen == 0)
			{
				return MakeNil();
			}
			{
				std::vector<wchar_t> buf(uLen + 1, 0);
				GetEnvironmentVariableW(name.c_str(), buf.data(), uLen + 1);
				return MakeStr(String(buf.data()));
			}
		}

		Value HostEnvSet(Interp&, std::vector<Value>& args, int line)
		{
			String name = ArgStr(args, 0, line, L"name");
			String value = (args.size() > 1) ? ToDisplay(args[1]) : String();

			// The call's own answer. It was dropped, so a name the process
			// cannot set -- an empty one, or one the system refuses -- was
			// traced as a success and the script carried on believing the
			// variable was there.
			if (!SetEnvironmentVariableW(name.c_str(), value.c_str()))
			{
				throw Error((long long) GetLastError(), String(L"env.set could not set ") + name, line);
			}
			TraceVerb(L"env.set", line, S_OK);
			return MakeNil();
		}

		// A real wait, unlike the `timeout /t` the shipped Post\Inidrive.bat used
		// (that one needs a console and returns at once under a PE launch, so its
		// "1 second" never happened).
		Value HostTimeSleep(Interp&, std::vector<Value>& args, int line)
		{
			long long ms = ArgInt(args, 0, line, L"milliseconds");

			// TimeDelay is the capability the legacy WAIT has always used, and it
			// keeps the message pump alive while it waits, so a splash or a tray
			// icon does not freeze. A bare Sleep would be a different operation.
			//
			// The legacy command answers anything <= 0 by waiting for a keypress
			// instead; that is a quirk of the text form, not something a caller
			// asking for a number of milliseconds can mean.
			if (ms > 0)
			{
				TimeDelay((DWORD) ms);
			}
			TraceVerb(String(L"time.sleep ") + ToText(MakeInt(ms)), line, S_OK);
			return MakeNil();
		}

		// --- display, fonts, the NumLock key ---------------------------------------

		// The four pure functions the core keeps: len, slice, find, replace.
		//
		// Everything else the dialect spelled as a command over strings lives in
		// the `str` library. These four stay in the core because they are the ones
		// every script reaches for, and because a library cannot be written
		// without them. None of them touches anything, so none of them traces:
		// the trace channel is for what a script *did*.

		// len(x) -- how many there are: characters in a string, elements in a
		// list, fields in a record. One verb rather than three, because it is the
		// same question asked of three shapes.
		Value HostLen(Interp&, std::vector<Value>& args, int line)
		{
			if (!args.empty())
			{
				switch (args[0].kind)
				{
				case VK_STR:    return MakeInt((long long) args[0].s.size());
				case VK_LIST:   return MakeInt(args[0].list ? (long long) args[0].list->size() : 0);
				case VK_RECORD: return MakeInt(args[0].rec ? (long long) args[0].rec->size() : 0);
				default:        break;
				}
			}
			throw Error(0x80070057, L"len takes a string, a list or a record", line);
		}

		// find(where, what) -- the position of `what` in `where`, counted from
		// zero, or nil when it is not there. nil rather than -1 because "not
		// there" is an answer a script tests for, and because a position that is
		// also an error is how an off-by-one becomes a silent one.
		Value HostFind(Interp&, std::vector<Value>& args, int line)
		{
			String where = ArgStr(args, 0, line, L"string");
			String what = ArgStr(args, 1, line, L"what to look for");
			size_t at = where.find(what);

			if (at == String::npos)
			{
				return MakeNil();
			}
			return MakeInt((long long) at);
		}

		// slice(x, start[, {count: n}]) -- a piece of a string or a list.
		//
		// `start` counts from zero and has to be inside the value; `count` is how
		// many to take and defaults to "to the end". A count that runs past the
		// end is shortened rather than refused, because that is what "take twenty
		// of these" means when there are twelve.
		Value HostSlice(Interp&, std::vector<Value>& args, int line)
		{
			long long start = ArgInt(args, 1, line, L"start");
			const Value *pv = Field(args, 2, L"count");
			long long count = -1;
			size_t total;

			if (pv)
			{
				if (pv->kind != VK_INT)
				{
					throw Error(0x80070057, L"slice: count must be a number", line);
				}
				count = pv->i;
				if (count < 0)
				{
					throw Error(0x80070057, L"slice: count cannot be negative", line);
				}
			}
			if (args.empty())
			{
				throw Error(0x80070057, L"slice takes a string or a list", line);
			}
			switch (args[0].kind)
			{
			case VK_STR:  total = args[0].s.size(); break;
			case VK_LIST: total = args[0].list ? args[0].list->size() : 0; break;
			default:
				throw Error(0x80070057, L"slice takes a string or a list", line);
			}
			if ((start < 0) || (start > (long long) total))
			{
				throw Error(0x80070057, L"slice: start is outside the value", line);
			}
			{
				size_t at = (size_t) start;
				size_t want = (count < 0) ? (total - at) : (size_t) count;

				if (want > (total - at))
				{
					want = total - at;
				}
				if (args[0].kind == VK_STR)
				{
					return MakeStr(args[0].s.substr(at, want));
				}
				{
					Value out;

					out.kind = VK_LIST;
					out.list = ListPtr(new std::vector<Value>(
						args[0].list->begin() + (long) at,
						args[0].list->begin() + (long) (at + want)));
					return out;
				}
			}
		}

		// replace(s, from, to) -- every occurrence, which is what the name means
		// and what the dialect's SED had to be told twice to do.
		//
		// The rest of what the dialect spelled as path commands is the `str`
		// library below: it is a library and not core because a script that
		// never touches a path should not carry it, and because `slice` and
		// `find` already answer for LSTR, RSTR, STRL and LPOS.
		Value HostReplace(Interp&, std::vector<Value>& args, int line)
		{
			String s = ArgStr(args, 0, line, L"string");
			String from = ArgStr(args, 1, line, L"what to replace");
			String to = ArgStr(args, 2, line, L"what to put there");
			String out;
			size_t at = 0;

			// An empty needle would match at every position and never advance.
			if (from.empty())
			{
				throw Error(0x80070057, L"replace: what to replace cannot be empty", line);
			}
			for (;;)
			{
				size_t hit = s.find(from, at);

				if (hit == String::npos)
				{
					out += s.substr(at);
					break;
				}
				out += s.substr(at, hit - at);
				out += to;
				at = hit + from.size();
			}
			return MakeStr(out);
		}

		// --- the str library: the path pieces the dialect spelled as commands ---

		// Windows does the splitting, not this code. UNC paths, drive-relative
		// paths, the two separator characters and a name with several dots are
		// each a case where splitting by hand looks right until it meets one.
		// A path is copied into a mutable buffer because these take a pointer to
		// write into, and the script's own string is not ours to edit.
		bool PathBuffer(const String& path, std::vector<wchar_t> *out)
		{
			out->assign(path.begin(), path.end());
			out->push_back(0);
			return !path.empty();
		}

		// str.dirname(path) -- everything up to the last separator, with no
		// trailing one (FDIR).
		Value HostStrDirname(Interp&, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"path");
			std::vector<wchar_t> buf;

			PathBuffer(path, &buf);
			PathRemoveFileSpecW(&buf[0]);
			return MakeStr(String(&buf[0]));
		}

		// str.filename(path) -- the last component, extension and all (FNAM).
		Value HostStrFilename(Interp&, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"path");
			std::vector<wchar_t> buf;

			PathBuffer(path, &buf);
			return MakeStr(String(PathFindFileNameW(&buf[0])));
		}

		// str.basename(path) -- the last component with its extension taken off
		// (NAME). It is the pair to str.extension: the two together are the name.
		Value HostStrBasename(Interp&, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"path");
			std::vector<wchar_t> buf;

			PathBuffer(path, &buf);
			{
				std::vector<wchar_t> name(PathFindFileNameW(&buf[0]), &buf[0] + buf.size());

				PathRemoveExtensionW(&name[0]);
				return MakeStr(String(&name[0]));
			}
		}

		// str.extension(path) -- without the dot, and empty when there is none
		// (FEXT). Not lowercased: a caller comparing extensions wants to say so,
		// and quietly changing the case would make that comparison a lie.
		Value HostStrExtension(Interp&, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"path");
			std::vector<wchar_t> buf;
			PCWSTR ptzDot;

			PathBuffer(path, &buf);
			ptzDot = PathFindExtensionW(&buf[0]);
			// "No extension" comes back as a pointer at the terminator, not as a
			// null pointer -- reading one past it would return whatever follows
			// the string in the buffer.
			if (!ptzDot || (*ptzDot == 0))
			{
				return MakeStr(String());
			}
			return MakeStr(String(ptzDot + 1));
		}

		// str.fields(s) -- the whitespace-separated fields of a string, as a list.
		//
		// This is what MSTR was for: "<2>" and "<#>" and the rest were one
		// function asked for one field at a time. A list answers all of it at
		// once, `len` counts them, and `slice` takes any of them.
		Value HostStrFields(Interp&, std::vector<Value>& args, int line)
		{
			String s = ArgStr(args, 0, line, L"string");
			Value out;
			size_t at = 0;

			out.kind = VK_LIST;
			out.list = ListPtr(new std::vector<Value>());
			while (at < s.size())
			{
				size_t start;

				while ((at < s.size()) && ((s[at] == L' ') || (s[at] == L'\t')))
				{
					at++;
				}
				start = at;
				while ((at < s.size()) && (s[at] != L' ') && (s[at] != L'\t'))
				{
					at++;
				}
				if (at > start)
				{
					out.list->push_back(MakeStr(s.substr(start, at - start)));
				}
			}
			return out;
		}

		// str.lines(s) -- split on line breaks, as a list.
		//
		// Splitting and reading stay separate: the caller reads the file
		// (fs.read) and splits it here, which is the same pair every other
		// reader wants -- and it keeps the file-reading verb from owning a
		// text-shape decision. A trailing break ends the last line rather than
		// starting an empty one, and a lone \r or a \r\n both count as one break.
		Value HostStrLines(Interp&, std::vector<Value>& args, int line)
		{
			String s = ArgStr(args, 0, line, L"string");
			std::vector<String> lines = SplitTextLines(s);
			Value out;

			out.kind = VK_LIST;
			out.list = ListPtr(new std::vector<Value>());
			for (size_t i = 0; i < lines.size(); i++)
			{
				out.list->push_back(MakeStr(lines[i]));
			}
			return out;
		}

		// sys.display({width, height, bits, hz}) -- put the screen in a mode.
		//
		// Every field is optional and what the caller does not name comes from the
		// mode that is on screen now, so "change only the depth" is expressible.
		// The answer is the DISP_CHANGE_* code as a number rather than a
		// success/failure: 0 means it took, 0xFFFFFFFE (DISP_CHANGE_BADMODE) means
		// no adapter has that mode, and a caller asking for a mode wants to be
		// told which of those happened, not stopped at the first.
		Value HostSysDisplay(Interp&, std::vector<Value>& args, int line)
		{
			DEVMODE dmOld;
			DEVMODE dmNew;
			const Value *v;

			ZeroMemory(&dmOld, sizeof(dmOld));
			if (!EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &dmOld))
			{
				throw Error((HRESULT) GetLastError(),
					L"sys.display: the current mode could not be read", line);
			}
			dmNew = dmOld;
			v = Field(args, 0, L"width");  if (v && (v->kind == VK_INT)) { dmNew.dmPelsWidth = (DWORD) v->i; }
			v = Field(args, 0, L"height"); if (v && (v->kind == VK_INT)) { dmNew.dmPelsHeight = (DWORD) v->i; }
			v = Field(args, 0, L"bits");   if (v && (v->kind == VK_INT)) { dmNew.dmBitsPerPel = (DWORD) v->i; }
			v = Field(args, 0, L"hz");     if (v && (v->kind == VK_INT)) { dmNew.dmDisplayFrequency = (DWORD) v->i; }
			{
				long long code = (long long) (LONG) SetNewDisplay(dmOld, dmNew);

				TraceVerb(String(L"sys.display ") + ToText(MakeInt(code)), line, S_OK);
				return MakeInt(code);
			}
		}

		// sys.numlock(on) -- leave the NumLock key in that state.
		//
		// The key is pressed only when the state differs from what was asked for;
		// pressing it when it is already right would flip it the other way. One
		// keystroke either way, so the caller does not have to know which way the
		// machine happened to start.
		Value HostSysNumlock(Interp&, std::vector<Value>& args, int line)
		{
			bool want = ArgBool(args, 0, line, L"on");
			BYTE abKeyState[256];

			if (!GetKeyboardState(abKeyState))
			{
				throw Error((HRESULT) GetLastError(),
					L"sys.numlock: the keyboard state could not be read", line);
			}
			if ((want && !(abKeyState[VK_NUMLOCK] & 1)) ||
				(!want && (abKeyState[VK_NUMLOCK] & 1)))
			{
				keybd_event(VK_NUMLOCK, 0x45, KEYEVENTF_EXTENDEDKEY, 0);
				keybd_event(VK_NUMLOCK, 0x45, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0);
			}
			TraceVerb(String(L"sys.numlock ") + (want ? L"on" : L"off"), line, S_OK);
			return MakeNil();
		}

		// sys.font(path) -- register the fonts in a file, or in every file of a
		// folder.
		//
		// The path is concrete. The command this replaces had a form that walked
		// every drive looking for a folder under each one's root -- the same shape
		// that was removed from LOAD, where a planted directory on a stick gets
		// its fonts registered. A caller that wants to look on several drives asks
		// fs.find_on_drives and names the one it means.
		Value HostSysFont(Interp&, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"path");
			DWORD dwAttr = GetFileAttributesW(path.c_str());
			TCHAR tzSpec[MAX_PATH];
			HRESULT hr;
			String spec;

			if (dwAttr == INVALID_FILE_ATTRIBUTES)
			{
				throw Error(ERROR_FILE_NOT_FOUND, String(L"sys.font: no such path: ") + path, line);
			}
			// A folder means every font in it, which is how a PE ships a font pack.
			spec = (dwAttr & FILE_ATTRIBUTE_DIRECTORY) ? (path + L"\\*.*") : path;
			if (!UStrCopyTo(tzSpec, spec.c_str()))
			{
				throw Error(ERROR_FILENAME_EXCED_RANGE, String(L"sys.font: path too long: ") + path, line);
			}
			hr = RegFont(tzSpec);
			if (hr != S_OK)
			{
				throw Error(hr, String(L"sys.font: no font was registered from ") + path, line);
			}
			TraceVerb(String(L"sys.font ") + path, line, S_OK);
			return MakeNil();
		}

		// time.now() -- the clock, as both a number and the parts a person reads.
		//
		// `epoch` is UTC seconds since 1970, which is what a script compares and
		// subtracts; the field names are the local time, which is what a script
		// puts in a file name or a log line. Both in one call, because asking for
		// one and then the other would be two clocks and possibly two seconds.
		// It replaces DATE. A wall clock is not deterministic, so this is the kind
		// of call the trace records for replay.
		Value HostTimeNow(Interp&, std::vector<Value>&, int line)
		{
			SYSTEMTIME stLocal;
			SYSTEMTIME stUtc;
			FILETIME ftUtc;
			Value v;

			GetLocalTime(&stLocal);
			GetSystemTime(&stUtc);
			SystemTimeToFileTime(&stUtc, &ftUtc);

			v.kind = VK_RECORD;
			v.rec = RecordPtr(new std::vector<std::pair<String, Value> >());
			RecPut(v, L"epoch",   MakeInt(FileTimeToEpoch(ftUtc)));
			RecPut(v, L"year",    MakeInt(stLocal.wYear));
			RecPut(v, L"month",   MakeInt(stLocal.wMonth));
			RecPut(v, L"day",     MakeInt(stLocal.wDay));
			RecPut(v, L"hour",    MakeInt(stLocal.wHour));
			RecPut(v, L"minute",  MakeInt(stLocal.wMinute));
			RecPut(v, L"second",  MakeInt(stLocal.wSecond));
			RecPut(v, L"weekday", MakeInt(stLocal.wDayOfWeek));	// 0 = Sunday
			TraceVerb(L"time.now", line, S_OK);
			return v;
		}

		// --- sys ------------------------------------------------------------------

		// Boot() is the capability the desktop context menu already reaches
		// through the legacy SHUT command: it broadcasts the end-session
		// messages and then asks ntdll, which does not return when it works. The
		// trace line is written first so a log that stops here says why.
		Value HostSysShutdown(Interp&, std::vector<Value>&, int line)
		{
			TraceVerb(L"sys.shutdown", line, S_OK);
			Boot(FALSE);
			return MakeNil();
		}

		Value HostSysReboot(Interp&, std::vector<Value>&, int line)
		{
			TraceVerb(L"sys.reboot", line, S_OK);
			Boot(TRUE);
			return MakeNil();
		}

		Value HostSysRecycle(Interp&, std::vector<Value>& args, int line)
		{
			String drive = ArgStr(args, 0, line, L"drive");
			long long mb = ArgInt(args, 1, line, L"capacity in MB");
			HRESULT hr = SetRecycleCapacity(drive.c_str(), (DWORD) ((mb > 0) ? mb : 0));

			// S_FALSE is "there was no volume to act on"; anything else is a
			// write that failed. Telling them apart matters: "no volume matched"
			// sends a reader looking at their drive letters, and a failed write
			// sends them looking at permissions.
			if (hr == S_FALSE)
			{
				throw Error(hr, String(L"sys.recycle_capacity: no volume matched ") + drive, line);
			}
			if (hr != S_OK)
			{
				throw Error(hr, String(L"sys.recycle_capacity: the setting could not be written for ") + drive, line);
			}
			TraceVerb(String(L"sys.recycle_capacity ") + drive, line, S_OK);
			return MakeNil();
		}


		// The two halves of INIT that the reference's "I" and "K" letters carried.
		// Each one records a request and nothing more: "I" says the tray icon is
		// wanted, "K" says the low-level keyboard hook is wanted, and the shell's
		// own startup reads the flag back and does the drawing or the installing.
		// So the verb writes the same registry DWORD to the same place the
		// reference's INIT wrote it, and the UI round owns what happens next.
		Value HostUiTrayIcon(Interp&, std::vector<Value>&, int line)
		{
			BOOL wanted = TRUE;
			HRESULT hr = RegSetValue(HKEY_LOCAL_MACHINE, PHOS_STATE, TEXT("TrayIcon"),
				REG_DWORD, &wanted, sizeof(wanted));

			if (hr != S_OK)
			{
				throw Error(hr, L"ui.tray_icon: the flag could not be written", line);
			}
			TraceVerb(L"ui.tray_icon", line, S_OK);
			return MakeNil();
		}

		Value HostUiHotkeyHook(Interp&, std::vector<Value>&, int line)
		{
			BOOL wanted = TRUE;
			HRESULT hr = RegSetValue(HKEY_LOCAL_MACHINE, PHOS_STATE, TEXT("InitProc"),
				REG_DWORD, &wanted, sizeof(wanted));

			if (hr != S_OK)
			{
				throw Error(hr, L"ui.hotkey_hook: the flag could not be written", line);
			}
			TraceVerb(L"ui.hotkey_hook", line, S_OK);
			return MakeNil();
		}

		Value HostSysFbwf(Interp&, std::vector<Value>& args, int line)
		{
			DWORD dwPercent = 0;
			DWORD dwMinMb = 0;
			DWORD dwMaxMb = 0;
			DWORD dwSize;
			HRESULT hr;
			const Value *v;

			// A percentage of 0 means "no percentage term", which is also what
			// an absent option means -- the legacy "P0" is mapped onto 1 by
			// GetNewSize before it reaches the same arithmetic.
			v = Field(args, 0, L"percent");
			if (v && (v->kind == VK_INT) && (v->i > 0)) { dwPercent = (DWORD) v->i; }
			v = Field(args, 0, L"min_mb");
			if (v && (v->kind == VK_INT) && (v->i > 0)) { dwMinMb = (DWORD) v->i; }
			v = Field(args, 0, L"max_mb");
			if (v && (v->kind == VK_INT) && (v->i > 0)) { dwMaxMb = (DWORD) v->i; }

			dwSize = FbwfCacheSize(dwPercent, dwMinMb, dwMaxMb,
				c_fbwfMinMb, c_fbwfMaxMb, c_fbwfDefaultMb);
			hr = ApplyFbwfCache(dwSize);
			// Already running is where the caller wants the service, and FBWF is
			// running in this PE by the time any script can ask for it. The
			// legacy command keeps reporting 1056 there; this is a postcondition.
			if ((hr != S_OK) && (hr != (HRESULT) ERROR_SERVICE_ALREADY_RUNNING))
			{
				// Two things can fail and the record carries the code that says
				// which: the value the service reads, or the service itself.
				throw Error(hr, L"sys.fbwf: the cache size is not in place (its value "
					L"could not be written, or the service would not start)", line);
			}
			// The size the arithmetic settled on is the interesting part, so it
			// goes in the trace rather than only into the registry.
			TraceVerb(String(L"sys.fbwf ") + ToText(MakeInt((long long) dwSize)), line, S_OK);
			return MakeNil();
		}

		Value HostSysPageFile(Interp&, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"page file path");
			const Value *v;
			long long minMb = 0;
			long long maxMb = 0;
			HRESULT hr;

			v = Field(args, 1, L"min_mb");
			if (v && (v->kind == VK_INT) && (v->i > 0)) { minMb = v->i; }
			v = Field(args, 1, L"max_mb");
			if (v && (v->kind == VK_INT) && (v->i > 0)) { maxMb = v->i; }
			if (maxMb < minMb) { maxMb = minMb; }

			// The reference only writes one when the machine needs it: free
			// memory has to be below the maximum being asked for. That is the
			// documented "use the initial size as the condition" behaviour.
			// { force: true } skips the test, which is the only way to ask for a
			// page file on a machine that does not need one.
			if (!OptBool(args, 1, L"force", false) && (GetFreeMemo() >= (DWORD) maxMb))
			{
				TraceVerb(String(L"sys.page_file ") + path + L" (not needed)", line, S_OK);
				return MakeNil();
			}
			hr = CreatePageFileAt(path.c_str(), (DWORD) minMb, (DWORD) maxMb);
			if (hr != S_OK)
			{
				throw Error(hr, String(L"sys.page_file failed for ") + path, line);
			}
			TraceVerb(String(L"sys.page_file ") + path, line, S_OK);
			return MakeNil();
		}

		Value HostDiskEject(Interp&, std::vector<Value>& args, int line)
		{
			String drive = ArgStr(args, 0, line, L"drive");
			long long retry = 0;
			BOOL bCDROM = TRUE;
			BOOL bUSB = TRUE;
			const Value *v;
			HRESULT hr;

			v = Field(args, 1, L"retry");
			if (v && (v->kind == VK_INT) && (v->i > 0)) { retry = v->i; }
			v = Field(args, 1, L"cdrom");
			if (v) { bCDROM = Truthy(*v) ? TRUE : FALSE; }
			v = Field(args, 1, L"usb");
			if (v) { bUSB = Truthy(*v) ? TRUE : FALSE; }

			// "*" or nothing is "every ejectable drive the flags allow". That is
			// a NULL name, not an empty one -- the capability tells them apart on
			// purpose, because the legacy text form produces both.
			//
			// bAsk is FALSE: a script gets an answer, never a question. Before
			// this the missing `retry` option meant "ask the operator", so a
			// headless eject that failed sat on a message box for ever.
			hr = EjectDrives((drive.empty() || (drive == L"*")) ? NULL : drive.c_str(),
				(UINT) retry, bCDROM, bUSB, FALSE);
			if (hr != S_OK)
			{
				throw Error(hr, String(L"disk.eject: nothing was ejected for ") + drive, line);
			}
			TraceVerb(String(L"disk.eject ") + drive, line, S_OK);
			return MakeNil();
		}

		Value HostDiskPublishLetters(Interp&, std::vector<Value>& args, int line)
		{
			String kind = ArgStr(args, 0, line, L"drive kind");
			UINT uType;
			UINT n;

			if (UStrCmpI((PTSTR) kind.c_str(), TEXT("cdrom")) == 0)
			{
				uType = DRIVE_CDROM;
			}
			else if ((UStrCmpI((PTSTR) kind.c_str(), TEXT("removable")) == 0) ||
				(UStrCmpI((PTSTR) kind.c_str(), TEXT("usb")) == 0))
			{
				uType = DRIVE_REMOVABLE;
			}
			else
			{
				throw Error(0x80070057,
					String(L"disk.publish_letters: expected \"cdrom\" or \"removable\", got ") + kind, line);
			}
			n = InitDriveList(uType);
			// The names it sets are the reference's -- CDROM, CDROM0 (the count)
			// and CDROM1.. -- and reading those back is what a script does.
			TraceVerb(String(L"disk.publish_letters ") + kind + L" n=" + ToText(MakeInt((long long) n)), line, S_OK);
			return MakeInt((long long) n);
		}

		Value HostDiskMountVolumes(Interp&, std::vector<Value>& args, int line)
		{
			String from = OptStr(args, 0, L"from");
			TCHAR cFrom = TEXT('C');
			INT n;

			// A drive letter, or nothing for "start at C". The range check is not
			// decoration: the letter becomes a bit index further down, and a digit
			// or a lower-case letter used to be shifted by a count the hardware
			// does not define. It lived in the text SHOW command before this verb
			// existed, so it moved here with the capability.
			if (!from.empty())
			{
				cFrom = from[0];
				if ((cFrom >= L'a') && (cFrom <= L'z'))
				{
					cFrom = (TCHAR) (cFrom - L'a' + L'A');
				}
				if ((cFrom < L'A') || (cFrom > L'Z'))
				{
					throw Error(0x80070057,
						L"disk.mount_volumes: { from } must be a drive letter A-Z", line);
				}
			}
			n = MountUnmountedVolumes(cFrom);

			if (n < 0)
			{
				throw Error(E_FAIL, L"disk.mount_volumes: the volume list could not be read", line);
			}
			// The count is the useful part: zero means every volume already had
			// a letter, which is the ordinary case on a machine that has booted.
			TraceVerb(String(L"disk.mount_volumes ") + ToText(MakeInt((long long) n)), line, S_OK);
			return MakeInt((long long) n);
		}

		Value HostDiskMountImage(Interp&, std::vector<Value>& args, int line)
		{
			String image = ArgStr(args, 0, line, L"image file");
			String mount = ArgStr(args, 1, line, L"mount folder");
			String tempDir = OptStr(args, 2, L"temp_dir");
			const Value *v = Field(args, 2, L"index");
			long long index = 1;
			HRESULT hr;

			if (v && (v->kind == VK_INT)) { index = v->i; }
			hr = MountWimImage(image.c_str(), mount.c_str(), (INT) index,
				tempDir.empty() ? NULL : tempDir.c_str());
			if (hr != S_OK)
			{
				throw Error(hr, String(L"disk.mount_image could not mount ") + image, line);
			}
			TraceVerb(String(L"disk.mount_image ") + image, line, S_OK);
			return MakeNil();
		}

		// The names a script reads, rather than the numbers the API answers with.
		const wchar_t *DriveTypeName(UINT uType)
		{
			switch (uType)
			{
			case DRIVE_FIXED:     return L"fixed";
			case DRIVE_REMOVABLE: return L"removable";
			case DRIVE_CDROM:     return L"cdrom";
			case DRIVE_RAMDISK:   return L"ramdisk";
			case DRIVE_REMOTE:    return L"remote";
			default:              return L"none";
			}
		}

		const wchar_t *BusTypeName(UINT uBus)
		{
			switch (uBus)
			{
			case BusTypeUsb:     return L"usb";
			case BusTypeSata:    return L"sata";
			case BusTypeAta:     return L"ata";
			case BusTypeScsi:    return L"scsi";
			case BusTypeVirtual: return L"virtual";
			default:             return L"unknown";
			}
		}

		// disk.volumes() -- every volume Windows knows about, as records.
		//
		// A script asks which volumes there are and what each one is, then names
		// the one it means by `id`, which is the name Windows uses
		// (\\?\Volume{GUID}\) and does not change when a letter does. The other
		// half -- disk.assign(id, letter) -- is not here yet: it replaces the
		// thing the boot chain uses to find the U: drive, and that has not been
		// proved against a real USB stick.
		Value HostDiskVolumes(Interp&, std::vector<Value>&, int line)
		{
			std::vector<VolumeInfo> volumes;
			Value out;

			if (!EnumVolumes(volumes))
			{
				throw Error(E_FAIL, L"disk.volumes: the volume list could not be read", line);
			}
			out.kind = VK_LIST;
			out.list = ListPtr(new std::vector<Value>());
			for (size_t i = 0; i < volumes.size(); i++)
			{
				Value v;

				v.kind = VK_RECORD;
				v.rec = RecordPtr(new std::vector<std::pair<String, Value> >());
				RecPut(v, L"id", MakeStr(volumes[i].tzName));
				RecPut(v, L"letter", volumes[i].tzLetter
					? MakeStr(String(1, volumes[i].tzLetter)) : MakeNil());
				RecPut(v, L"label", MakeStr(volumes[i].tzLabel));
				RecPut(v, L"drive_type", MakeStr(DriveTypeName(volumes[i].uDriveType)));
				RecPut(v, L"bus", MakeStr(BusTypeName(volumes[i].uBusType)));
				RecPut(v, L"size_mb", MakeInt((long long) volumes[i].uTotalMb));
				RecPut(v, L"free_mb", MakeInt((long long) volumes[i].uFreeMb));
				out.list->push_back(v);
			}
			TraceVerb(String(L"disk.volumes n=")
				+ ToText(MakeInt((long long) volumes.size())), line, S_OK);
			return out;
		}

		// disk.assign(id, letter) -- give a volume a letter, or take its letters
		// off. `id` is what disk.volumes() answers with, so a script names the
		// volume it means instead of describing it and hoping.
		//
		// A letter another volume already holds is refused rather than taken:
		// both volumes are visible in disk.volumes(), so the script can say what
		// should happen to the other one. nil (or an empty string) means "no
		// letter".
		Value HostDiskAssign(Interp&, std::vector<Value>& args, int line)
		{
			String id = ArgStr(args, 0, line, L"volume id");
			String letter;
			TCHAR cLetter = 0;
			HRESULT hr;

			if (args.size() > 1)
			{
				if (args[1].kind == VK_STR)
				{
					letter = args[1].s;
				}
				else if (args[1].kind != VK_NIL)
				{
					throw Error(0x80070057,
						L"disk.assign: the letter must be a string or nil", line);
				}
			}
			if (!letter.empty())
			{
				if (letter.size() != 1)
				{
					throw Error(0x80070057, L"disk.assign: the letter is one character", line);
				}
				cLetter = letter[0];
			}

			hr = SetVolumeLetter(id.c_str(), cLetter);
			if (hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED))
			{
				throw Error(hr, L"disk.assign: that is the volume this system is running "
					L"from, and its letter is not a thing to move", line);
			}
			if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_ASSIGNED))
			{
				throw Error(hr, String(L"disk.assign: ") + letter
					+ L" is already another volume's letter", line);
			}
			if (hr != S_OK)
			{
				throw Error(hr, String(L"disk.assign: ") + id, line);
			}
			TraceVerb(String(L"disk.assign ") + id + L" "
				+ (letter.empty() ? String(L"(no letter)") : letter), line, S_OK);
			return MakeNil();
		}

		// disk.unmount_image({image} or {mount}, {commit})
		//
		// The library takes either name, and the caller has one of them: a script
		// that mounted an image knows the file, one that inherited a mount knows
		// the folder. Mounting and unmounting are separate verbs because they are
		// separate decisions -- the text MOUN answered "take it down" only when a
		// field was left out, which is a spelling nobody can read. { commit: true }
		// keeps what was written into the image; the default discards it, which is
		// what the dialect always did.
		Value HostDiskUnmountImage(Interp&, std::vector<Value>& args, int line)
		{
			String image = OptStr(args, 0, L"image");
			String mount = OptStr(args, 1, L"mount");
			String name = !image.empty() ? image : mount;
			bool commit = OptBool(args, 1, L"commit", false);
			HRESULT hr;

			if (name.empty())
			{
				throw Error(0x80070057,
					L"disk.unmount_image: name the image or the folder it is mounted at",
					line);
			}
			hr = UnmountWimImage(name.c_str(), commit ? TRUE : FALSE);
			if (hr != S_OK)
			{
				throw Error(hr,
					String(L"disk.unmount_image: nothing was unmounted for ") + name, line);
			}
			TraceVerb(String(L"disk.unmount_image ") + name, line, S_OK);
			return MakeNil();
		}

		Value HostDiskRamDisk(Interp&, std::vector<Value>& args, int line)
		{
			const Value *v = Field(args, 0, L"size_mb");
			DWORD dwWanted = 0;
			DWORD dwSizeMb;
			HRESULT hr;

			if (v && (v->kind == VK_INT) && (v->i > 0)) { dwWanted = (DWORD) v->i; }
			// The same clamping the legacy command reaches through GetNewSize:
			// 1 to 512 MB, and 32 MB when nothing is asked for.
			dwSizeMb = FbwfCacheSize(0, dwWanted, dwWanted, 1, 512, 32);
			hr = SetRamDiskSizeMb(dwSizeMb);
			if (hr != S_OK)
			{
				throw Error(hr, L"disk.ram_disk: the ramdisk drive is not there", line);
			}
			TraceVerb(String(L"disk.ram_disk ") + ToText(MakeInt((long long) dwSizeMb)), line, S_OK);
			return MakeNil();
		}

		// --- rand ----------------------------------------------------------------

		// The one place the language is allowed to be unpredictable, and it is a
		// call rather than a variable. The reference had %RANDOM%, a built-in
		// that quietly changed value on every expansion; here the draw is
		// explicit, and the number it produced goes into the trace so a run can
		// be read back and the same sequence replayed with rand.seed.
		ULONGLONG g_randState = 0;

		ULONGLONG NextRandom()
		{
			if (!g_randState)
			{
				LARGE_INTEGER counter;

				// GetTickCount rather than GetTickCount64: the headers this
				// project pins are older than that one's introduction, so it is
				// not even declared here.
				QueryPerformanceCounter(&counter);
				g_randState = ((ULONGLONG) counter.QuadPart << 17)
					^ GetTickCount() ^ 0x9E3779B97F4A7C15ull;
				if (!g_randState)
				{
					g_randState = 0x2545F4914F6CDD1Dull;
				}
			}
			// xorshift64*. Short, well behaved enough for picking a wallpaper,
			// and it asks the system for nothing beyond the seed.
			g_randState ^= g_randState >> 12;
			g_randState ^= g_randState << 25;
			g_randState ^= g_randState >> 27;
			return g_randState * 0x2545F4914F6CDD1Dull;
		}


		Value HostRandRange(Interp&, std::vector<Value>& args, int line)
		{
			long long lo = ArgInt(args, 0, line, L"low bound");
			long long hi = ArgInt(args, 1, line, L"high bound");
			long long drawn;

			if (hi <= lo)
			{
				throw Error(0x80070057, L"rand.range: the low bound must be below the high one", line);
			}
			drawn = lo + (long long) (NextRandom() % (ULONGLONG) (hi - lo));
			TraceVerb(String(L"rand.range ") + ToText(MakeInt(drawn))
				+ L" in " + ToText(MakeInt(lo)) + L".." + ToText(MakeInt(hi)), line, S_OK);
			return MakeInt(drawn);
		}

		Value HostRandPick(Interp&, std::vector<Value>& args, int line)
		{
			if (args.empty() || (args[0].kind != VK_LIST) || !args[0].list)
			{
				throw Error(0x80070057, L"rand.pick wants a list", line);
			}
			if (args[0].list->empty())
			{
				throw Error(0x80070057, L"rand.pick: the list is empty", line);
			}
			{
				size_t at = (size_t) (NextRandom() % (ULONGLONG) args[0].list->size());

				// The index is the draw, so that is what goes into the trace --
				// an element could be a page of text.
				TraceVerb(String(L"rand.pick index ") + ToText(MakeInt((long long) at)), line, S_OK);
				return (*args[0].list)[at];
			}
		}

		Value HostRandSeed(Interp&, std::vector<Value>& args, int line)
		{
			long long seed = ArgInt(args, 0, line, L"seed");

			// Zero means "no state yet" to NextRandom, so a zero seed would just
			// reseed from the clock. Fold it to something non-zero instead, which
			// is what makes rand.seed(0) reproducible too.
			g_randState = seed ? (ULONGLONG) seed : 0x9E3779B97F4A7C15ull;
			TraceVerb(String(L"rand.seed ") + ToText(MakeInt(seed)), line, S_OK);
			return MakeNil();
		}

		// --- run (load another script at run time) --------------------------------

		// run(path, { wait: false }) puts the whole script in its own process, so
		// proc.await and proc.await_all treat a script exactly like an external
		// program. "Drop a file in a folder and let it run beside the others" is
		// the shape this exists for, and it is why the task gets its own trail
		// instead of sharing the parent's.
		Value RunAsSubprocess(Interp& in, const String& path, int line)
		{
			TCHAR tzExe[MAX_PATH];

			if (GetModuleFileName(NULL, tzExe, MAX_PATH) == 0)
			{
				throw Error((HRESULT) GetLastError(), L"run: cannot locate this executable", line);
			}
			{
				ProcSpec spec;
				HANDLE hProcess = NULL;
				DWORD dwPid = 0;
				String trail = ChildTrailPath(path);

				spec.cmd = QuoteAlways(tzExe) + L" phos run " + Quote(path);
				spec.hidden = true;
				spec.env.push_back(std::make_pair(String(L"PHOS_DEBUG_LOG"), trail));

				SpawnAndTrace(L"run", spec, false, line, &hProcess, &dwPid);

				if (in.procs.size() < MAXIMUM_WAIT_OBJECTS)
				{
					in.procs[(long long) dwPid] = hProcess;
					in.procLogs[(long long) dwPid] = trail;
				}
				else
				{
					CloseHandle(hProcess);
				}
				return MakeProcValue(dwPid);
			}
		}

		// Is this path on media that can be swapped under us?
		//
		// A disc counts with a stick: both are replaceable by whoever is standing
		// at the machine. MyGetDriveType already treats a bridge that claims to be
		// a fixed disk as removable when the device says it is hot pluggable,
		// which is the same question asked one layer down.
		//
		// DRIVE_RAMDISK is deliberately not on the list: the boot image itself is
		// there, so calling it removable would refuse the chain that is running.
		bool IsRemovablePath(const String& path)
		{
			TCHAR tzRoot[MAX_PATH];

			if (!GetVolumePathNameW(path.c_str(), tzRoot, _NumOf(tzRoot)))
			{
				return false;		// no root to judge; the open below reports it
			}
			{
				UINT uType = MyGetDriveType(tzRoot);

				return (uType == DRIVE_REMOVABLE) || (uType == DRIVE_CDROM);
			}
		}

		Value HostRun(Interp& in, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"script path");
			bool wait = OptBool(args, 1, L"wait", true);
			bool trusted = OptBool(args, 1, L"trusted", false);
			HRESULT hr;

			if (!UFileExist((PTSTR) path.c_str()))
			{
				throw Error(ERROR_FILE_NOT_FOUND, L"run: no such script: " + path, line);
			}
			// A script on media that can be swapped under us is a script anyone
			// who can plug a stick in can replace, and this one runs with the
			// rights to change the machine. The caller has to say it means it.
			if (!trusted && IsRemovablePath(path))
			{
				throw Error((HRESULT) ERROR_ACCESS_DENIED,
					String(L"run: ") + path + L" is on removable media; pass { trusted: true }"
					L" if that is intended", line);
			}
			if (!wait)
			{
				return RunAsSubprocess(in, path, line);
			}
			hr = LoadFile((PTSTR) path.c_str());
			// Same trap as above: a phos error carries a Win32 code, and those
			// are positive. Only S_OK means the child script ran clean.
			if (hr != S_OK)
			{
				TCHAR tzBuf[128];
				UStrPrint(tzBuf, TEXT("run %s failed (0x%08X)"), path.c_str(), (UINT) hr);
				throw Error(hr, tzBuf, line);
			}
			// One trace line per loaded script, so the trace shows which Post
			// files actually ran rather than only what they did.
			TraceVerb(String(L"run ") + path, line, hr);
			return MakeNil();
		}

		// --- disk -----------------------------------------------------------------

		// disk.letter("U", { bus: "usb" })
		//
		// Give the volume on a given bus a drive letter. This is the job
		// disposition\FixUSB.exe did; AssignLetterToBus documents exactly what is
		// and is not the same about the result.
		Value HostDiskLetter(Interp&, std::vector<Value>& args, int line)
		{
			String text = ArgStr(args, 0, line, L"drive letter");
			String bus = OptStr(args, 1, L"bus");
			String msg;
			TCHAR c;
			HRESULT hr;

			if (bus.empty())
			{
				bus = L"usb";
			}
			if (text.size() != 1)
			{
				throw Error(ERROR_INVALID_PARAMETER, L"disk.letter: the letter is one character", line);
			}
			c = text[0];
			if ((c >= L'a') && (c <= L'z'))
			{
				c = (TCHAR) (c - L'a' + L'A');
			}
			// Anything else would be turned into a mount point like "1:" and
			// reported as a failure from somewhere further down, which reads as
			// "the bus was wrong" rather than "that is not a drive letter".
			if ((c < L'A') || (c > L'Z'))
			{
				throw Error(ERROR_INVALID_PARAMETER,
					L"disk.letter: the letter must be A-Z", line);
			}

			hr = AssignLetterToBus(c, (PTSTR) bus.c_str());
			if (hr != S_OK)
			{
				msg = L"disk.letter " + String(1, c) + L": on bus " + bus;
				throw Error(hr, msg, line);
			}
			TraceVerb(L"disk.letter", line, hr);
			return MakeNil();
		}

		const HostEntry kHost[] =
		{
			{ L"log.open",             HostLogOpen, SE_READ },
			{ L"log.close",            HostLogClose, SE_READ },
			{ L"log.info",             HostLogInfo, SE_READ },
			{ L"log.warn",             HostLogWarn, SE_READ },
			{ L"log.error",            HostLogError, SE_READ },
			{ L"debug.desktop",        HostDebugDesktop, SE_READ },
			{ L"fail",                 HostFail, SE_READ },
			{ L"len",                  HostLen, SE_READ },
			{ L"find",                 HostFind, SE_READ },
			{ L"slice",                HostSlice, SE_READ },
			{ L"replace",              HostReplace, SE_READ },
			{ L"str.dirname",          HostStrDirname, SE_READ },
			{ L"str.filename",         HostStrFilename, SE_READ },
			{ L"str.basename",         HostStrBasename, SE_READ },
			{ L"str.extension",        HostStrExtension, SE_READ },
			{ L"str.fields",           HostStrFields, SE_READ },
#ifdef PHOS_TESTHOOKS
			{ L"debug.crash",          HostDebugCrash, SE_READ },
			{ L"debug.env_count",      HostDebugEnvCount, SE_READ },
			{ L"debug.copy_count",     HostDebugCopyCount, SE_READ },
			{ L"debug.step_limit",     HostDebugStepLimit, SE_READ },
#endif
			{ L"proc.run",             HostProcRun, SE_WRITE },
			{ L"proc.spawn",           HostProcSpawn, SE_WRITE },
			{ L"proc.kill",            HostProcKill, SE_WRITE },
			{ L"proc.exists",          HostProcExists, SE_READ },
			{ L"proc.await",           HostProcAwait, SE_WRITE },
			{ L"proc.await_all",       HostProcAwaitAll, SE_WRITE },
			{ L"reg.set",              HostRegSet, SE_WRITE },
			{ L"reg.delete",           HostRegDelete, SE_WRITE },
			{ L"reg.delete_tree",      HostRegDeleteTree, SE_WRITE },
			{ L"reg.get",              HostRegGet, SE_READ },
			{ L"rand.range",           HostRandRange, SE_READ },
			{ L"rand.pick",            HostRandPick, SE_READ },
			{ L"rand.seed",            HostRandSeed, SE_READ },
			{ L"reg.load_hive",        HostRegLoadHive, SE_WRITE },
			{ L"reg.unload_hive",      HostRegUnloadHive, SE_WRITE },
			{ L"fs.exists",            HostFsExists, SE_READ },
			{ L"fs.stat",              HostFsStat, SE_READ },
			{ L"fs.hash",              HostFsHash, SE_READ },
			{ L"fs.find_on_drives",    HostFsFindOnDrives, SE_READ },
			{ L"fs.list",              HostFsList, SE_READ },
			{ L"fs.list_dirs",         HostFsListDirs, SE_READ },
			{ L"fs.delete",            HostFsDelete, SE_WRITE },
			{ L"fs.mkdir",             HostFsMkdir, SE_WRITE },
			{ L"fs.read",              HostFsRead, SE_READ },
			{ L"str.lines",            HostStrLines, SE_READ },
			{ L"fs.write",             HostFsWrite, SE_WRITE },
			{ L"fs.append",            HostFsAppend, SE_WRITE },
			{ L"fs.rename",            HostFsRename, SE_WRITE },
			{ L"fs.hardlink",          HostFsHardlink, SE_WRITE },
			{ L"fs.temp_dir",          HostFsTempDir, SE_READ },
			{ L"fs.chdir",             HostFsChdir, SE_WRITE },
			{ L"fs.clean_temp",        HostFsCleanTemp, SE_WRITE },
			{ L"fs.edit_line",         HostFsEditLine, SE_WRITE },
			{ L"fs.set_attr",          HostFsSetAttr, SE_WRITE },
			{ L"shell.shortcut",       HostShellShortcut, SE_WRITE },
			{ L"shell.start",          HostShellStart, SE_WRITE },
			{ L"ui.pin_to_taskbar", HostUiPinToTaskbar, SE_WRITE },
			{ L"ui.folder_option", HostUiFolderOption, SE_WRITE },
			{ L"ui.wallpaper", HostUiWallpaper, SE_WRITE },
			{ L"ui.hotkey",          HostUiHotkey, SE_WRITE },
			{ L"ui.splash",            HostUiSplash, SE_WRITE },
			{ L"ui.splash_text",            HostUiSplashText, SE_WRITE },
			{ L"ui.splash_clear",           HostUiSplash, SE_WRITE },
			{ L"native.call",          HostNativeCall, SE_WRITE },
			{ L"native.load",          HostNativeLoad, SE_WRITE },
			{ L"drv.load",             HostDrvLoad, SE_WRITE },
			{ L"svc.start",            HostSvcStart, SE_WRITE },
			{ L"svc.stop",             HostSvcStop, SE_WRITE },
			{ L"svc.query",            HostSvcQuery, SE_READ },
			{ L"svc.create",           HostSvcCreate, SE_WRITE },
			{ L"svc.delete",           HostSvcDelete, SE_WRITE },
			{ L"disk.letter",          HostDiskLetter, SE_WRITE },
			{ L"disk.eject",           HostDiskEject, SE_WRITE },
			{ L"disk.mount_volumes",   HostDiskMountVolumes, SE_WRITE },
			{ L"disk.publish_letters", HostDiskPublishLetters, SE_WRITE },
			{ L"disk.mount_image",     HostDiskMountImage, SE_WRITE },
			{ L"disk.unmount_image",   HostDiskUnmountImage, SE_WRITE },
			{ L"disk.volumes",         HostDiskVolumes, SE_READ },
			{ L"disk.assign",          HostDiskAssign, SE_WRITE },
			{ L"disk.ram_disk",        HostDiskRamDisk, SE_WRITE },
			{ L"env.get",              HostEnvGet, SE_READ },
			{ L"env.set",              HostEnvSet, SE_WRITE },
			{ L"time.sleep",           HostTimeSleep, SE_WRITE },
			{ L"time.now",             HostTimeNow, SE_READ },
			{ L"sys.shutdown",         HostSysShutdown, SE_WRITE },
			{ L"sys.reboot",           HostSysReboot, SE_WRITE },
			{ L"sys.recycle_capacity", HostSysRecycle, SE_WRITE },
			{ L"ui.tray_icon",        HostUiTrayIcon, SE_WRITE },
			{ L"ui.hotkey_hook",      HostUiHotkeyHook, SE_WRITE },
			{ L"sys.fbwf",             HostSysFbwf, SE_WRITE },
			{ L"sys.page_file",        HostSysPageFile, SE_WRITE },
			{ L"sys.display",          HostSysDisplay, SE_WRITE },
			{ L"sys.numlock",          HostSysNumlock, SE_WRITE },
			{ L"sys.font",             HostSysFont, SE_WRITE },
			{ L"run",                  HostRun, SE_WRITE },
			{ NULL, NULL }
		};
	}	// namespace

	// Run what a hotkey was bound to. Called from the window procedure, so there
	// is no interpreter frame on the stack to call into -- the binding carries its
	// own interpreter, which is what `owner` is for.
	//
	// Outside the anonymous namespace on purpose: tray.cpp declares this one, and
	// a function with internal linkage cannot be the one that declaration names.
	//
	// A callback gets its own step budget, and an error inside one is logged and
	// swallowed. Letting it out would unwind through the window's message loop,
	// and the window would go with it.
	void RunHotkeyCallback(size_t slot)
	{
		std::shared_ptr<Interp> owner;
		FuncPtr fn;
		int line = 0;

		if (slot >= s_hotkeys.size())
		{
			return;		// a slot the menu owns, or one nobody bound
		}
		owner = s_hotkeys[slot].owner;
		fn = s_hotkeys[slot].fn;
		line = s_hotkeys[slot].line;
		if (!owner || !fn)
		{
			return;
		}
		owner->steps = 0;		// a fresh budget for this callback
		try
		{
			std::vector<Value> none;

			CallFunc(*owner, fn, none, line);
		}
		catch (Error& e)
		{
			TCHAR tzBuf[MAX_STR];

			UStrPrint(tzBuf, TEXT("hotkey %u (bound at line %d): %s"),
				(UINT) slot, line, e.message.c_str());
			SetLogs(tzBuf, (HRESULT) e.code);
		}
		catch (ReturnSignal&)
		{
			// A 'return' in a callback is just the end of it.
		}
		catch (GuardSignal& g)
		{
			// The watchdog is not catchable by a script, but the message loop
			// still has to say something: a callback that ran away is exactly
			// the case this channel exists for.
			TCHAR tzBuf[MAX_STR];

			UStrPrint(tzBuf, TEXT("hotkey %u (bound at line %d): %s"),
				(UINT) slot, line, g.message.c_str());
			SetLogs(tzBuf, (HRESULT) g.code);
		}
		catch (...)
		{
			// Anything the interpreter does not model. The message loop has to
			// survive what a script did.
		}
	}

	const HostEntry *FindHost(const String& name)
	{
		for (const HostEntry *e = kHost; e->name; e++)
		{
			if (name == e->name)
			{
				return e;
			}
		}
		return NULL;
	}

	BOOL HostHasVerb(const String& name)
	{
		return FindHost(name) != NULL;
	}

	Value CallHost(Interp& in, const String& name, std::vector<Value>& args, int line)
	{
		const HostEntry *e = FindHost(name);

		if (!e)
		{
			throw Error(0x80070002, String(L"unknown host function '") + name + L"'", line);
		}
			// The grade is read off the entry that was just found. It used to go
			// through a helper that looked the same name up a second time.
			if (in.dryRun && (e->side != SE_READ))
		{
			TraceVerb(String(L"(dry-run) ") + name, line, S_OK);
			return MakeNil();
		}
		// A "begin" line before the call and the command's own line after it.
		// Without the first one a boot that stops mid-call looks exactly like a
		// boot that finished - there is simply no further trace.
		TraceVerb(String(L"begin ") + name, line, S_OK);
		// Remember which verb is running: the catch block a script writes reads
		// this out of the error record, and the crash recorder's breadcrumb
		// points at the host table's own string, which is static and outlives
		// everything. Nothing here points at a local string: the breadcrumb is
		// read by a handler that runs after these frames are gone.
		{
			String previous = in.verb;

			in.verb = name;
			CrashSetVerb(e->name);
			try
			{
				Value result = e->fn(in, args, line);

				in.verb = previous;
				CrashSetVerb(NULL);
				return result;
			}
			catch (Error& err)
			{
				in.verb = previous;
				CrashSetVerb(NULL);
				// Stamp the name on the error before it leaves: the catch block
				// that reads the record runs after this call has returned, so
				// nothing else still knows which verb it was.
				if (err.verb.empty())
				{
					// The innermost verb wins: an outer call must not relabel an
					// error its callee already named.
					err.verb = name;
				}
				if (IsOptional(args))
				{
					TraceVerb(name + L" (optional) " + BriefArg(args), line, (HRESULT) err.code);
					return MakeNil();
				}
				throw;
			}
			catch (...)
			{
				in.verb = previous;
				CrashSetVerb(NULL);
				throw;
			}
		}
	}

	void RegisterBuiltins(Interp& in)
	{
		Value paths;

		paths.kind = VK_RECORD;
		paths.rec = RecordPtr(new std::vector<std::pair<String, Value> >());
		{
			String windir = EnvValue(L"WinDir");
			String pf = EnvValue(L"ProgramFiles");
			struct { const wchar_t *key; const wchar_t *env; String suffix; } const rows[] =
			{
				{ L"windir",           NULL,            String() },
				{ L"system32",         L"WinDir",       String(L"\\System32") },
				{ L"program_files",    NULL,            String() },
				{ L"program_files_x86", L"ProgramFiles(x86)", String() },
				{ L"desktop",          L"Desktop",      String() },
				{ L"programs",         L"Programs",     String() },
				{ L"start_menu",       L"StartMenu",    String() },
				{ L"startup",          L"Startup",      String() },
				{ L"favorites",        L"Favorites",    String() },
				{ L"personal",         L"Personal",     String() },
				{ L"send_to",          L"SendTo",       String() },
				{ L"quick_launch",     L"QuickLaunch",  String() },
				{ L"temp",             L"TEMP",         String() },
				{ L"public",           L"PUBLIC",       String() },
				{ L"users",            L"USERPROFILE",  String() },
			};

			for (size_t i = 0; i < _countof(rows); i++)
			{
				String value;

				if (rows[i].env)
				{
					value = EnvValue(rows[i].env);
				}
				if ((rows[i].key == String(L"windir")) && value.empty())
				{
					value = windir;
				}
				if ((rows[i].key == String(L"program_files")) && value.empty())
				{
					value = pf;
				}
				value += rows[i].suffix;
				paths.rec->push_back(std::make_pair(String(rows[i].key), MakeStr(value)));
			}
			{
				String disp = windir + L"\\System32\\disposition";
				paths.rec->push_back(std::make_pair(String(L"disposition"), MakeStr(disp)));
			}
		}
		in.globals->vars[L"path"] = paths;

		// script.dir and script.file: where the running script is. The
		// interpreter has carried both since RunBytes started recording them; a
		// script had no way to ask. It is what a relative path in a script means,
		// and what a message about "that file, the one next to me" needs.
		{
			Value script;

			script.kind = VK_RECORD;
			script.rec = RecordPtr(new std::vector<std::pair<String, Value> >());
			script.rec->push_back(std::make_pair(String(L"dir"), MakeStr(in.scriptDir)));
			script.rec->push_back(std::make_pair(String(L"file"), MakeStr(in.scriptPath)));
			in.globals->vars[L"script"] = script;
		}
	}
}
