//////////////////////////////////////////////////////////////////////////////////////////////////
// Phosphor host functions.
//
// A verb and the text command with the same job reach the same implementation
// through the capability layer below, not through a command line: a script
// calls FsDelete() where a command file calls FILE, and the two meet inside
// platform/. The one exception is hotkey.bind, whose command stays text because
// the hotkey handler that runs it later has no interpreter frame to call back
// into.
//
// The exceptions are the things no text command covers: file attributes,
// directory listing, environment reads and the path record. Those talk to
// Win32 directly and say so in their comments.
//////////////////////////////////////////////////////////////////////////////////////////////////
namespace Lang
{
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

		// Quote for CreateProcess / the text-command parsers: only when the text has a
		// space, so a plain X:\Windows\... stays readable in the trace.
		String Quote(const String& s)
		{
			if (s.find(L' ') == String::npos)
			{
				return s;
			}
			return L"\"" + s + L"\"";
		}

		// CreateProcess gets the whole line as lpCommandLine with no
		// lpApplicationName, and " X:\Program Files\x.exe" (a quoted token after
		// a leading space) is not parsed the way it looks: the space is part of
		// the first token, the quote is not at the start of the string, and the
		// call fails. Always quote the program and put it straight after the
		// EXEC flags, the way the text form writes an EXEC line.
		String QuoteAlways(const String& s)
		{
			return L"\"" + s + L"\"";
		}

		HRESULT RunTextCommand(const String& text, int line)
		{
			// The text-command handlers assume they own a big writable buffer: LINK
			// appends ".LNK" to the argument in place, and the parsers split on
			// NULs they write themselves. Give them room rather than an exactly
			// sized copy, or that append is a heap overflow.
			std::vector<TCHAR> buf(text.begin(), text.end());

			buf.resize(text.size() + 4096, 0);
			g_ctx.uLineNo = (UINT) line;
			return Command(buf.data());
		}

		void RunTextCommandOk(const String& text, int line)
		{
			HRESULT hr = RunTextCommand(text, line);
			if (FAILED(hr))
			{
				TCHAR tzBuf[128];
				UStrPrint(tzBuf, TEXT("%s failed (0x%08X)"), text.c_str(), (UINT) hr);
				throw Error(hr, tzBuf, line);
			}
		}

		// The verbs that change something on disk or in the registry, for
		// --dry-run. Names, not a table of members, because the language is
		// still moving and a missing entry must not silently count as read only.
		bool IsWriteVerb(const String& name)
		{
			return (name == L"env.set") || (name == L"proc.run") ||
				(name == L"proc.spawn") ||
				(name == L"disk.letter") ||
				(name == L"disk.eject") ||
				(name == L"disk.mount_volumes") ||
				(name == L"disk.publish_letters") ||
				(name == L"disk.mount_image") || (name == L"disk.ram_disk") ||
				(name == L"sys.shutdown") || (name == L"sys.reboot") ||
				(name == L"sys.recycle_capacity") ||
				(name == L"proc.kill") || (name == L"proc.await") ||
				(name == L"proc.await_all") || (name == L"reg.set") ||
				(name == L"reg.delete") || (name == L"reg.delete_tree") ||
				(name == L"reg.load_hive") || (name == L"reg.unload_hive") ||
				(name == L"fs.delete") || (name == L"fs.mkdir") ||
				(name == L"fs.write") || (name == L"fs.append") ||
				(name == L"fs.rename") || (name == L"fs.hardlink") ||
				(name == L"fs.chdir") ||
				(name == L"fs.clean_temp") || (name == L"fs.edit_line") ||
				(name == L"fs.set_attr") || (name == L"fs.hide") ||
				(name == L"shell.shortcut") || (name == L"shell.hide_window") ||
				(name == L"shell.start") || (name == L"shell.pin_to_taskbar") ||
				(name == L"hotkey.bind") || (name == L"boot.logo") ||
				(name == L"boot.text") || (name == L"boot.clear") ||
				(name == L"boot.add_run") || (name == L"boot.remove_run") ||
				(name == L"sys.owner") ||
				(name == L"sys.tray_icon") || (name == L"sys.hotkey_hook") ||
				(name == L"sys.fbwf") ||
				(name == L"sys.page_file") ||
				(name == L"native.call") || (name == L"native.load") ||
				(name == L"desktop.set_wallpaper") || (name == L"desktop.folder_option") ||
				(name == L"drv.load") || (name == L"svc.start") || (name == L"run") ||
				(name == L"svc.stop") ||
				(name == L"svc.create") || (name == L"svc.delete") ||
				(name == L"time.sleep");		// a dry run should not sit and wait
		}

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
			Logs(g_ctx, buf.data());
			return MakeNil();
		}

		Value HostLogClose(Interp&, std::vector<Value>&, int line)
		{
			Logs(g_ctx, TEXT(""));
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

		// --- proc -----------------------------------------------------------------

		// Starting a program does not go through the EXEC text command.
		//
		// Routing it through that command is a trap worth writing down:
		// Command() trims a fully quoted argument, but its check is "the first
		// character is a quote -> drop it, then drop the last character if that
		// is also a quote". A normal command line of the form
		//   "X:\Program Files\App\App.exe" -arg
		// therefore comes out as
		//   X:\Program Files\App\App.exe" -arg
		// with the opening quote gone and the closing one left behind, and
		// CreateProcess answers 2. A line that always writes a flag first
		// (`EXEC !"..."` / `EXEC -hide "..."`) never notices; a plain launch is
		// where it breaks.
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

		HRESULT SpawnProcess(const ProcSpec& spec, bool wait, HANDLE *phProcess, DWORD *pdwPid)
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
				WaitForSingleObject(pi.hProcess, INFINITE);
			}
			SetProcessWorkingSetSize(CurrentProcess, -1, -1);
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
			HANDLE *phProcess, DWORD *pdwPid)
		{
			HRESULT hr = SpawnProcess(spec, wait, phProcess, pdwPid);

			TraceVerb(verb + L" " + spec.cmd, line, hr);
			// hr != S_OK, not FAILED(hr): a failed CreateProcess reports
			// GetLastError(), and a Win32 error code is *positive* (2, 87), so
			// FAILED() - which only looks at the sign bit - says "fine". That is
			// how two regedit launches against a PE with no regedit.exe came out
			// as trace lines nobody acted on.
			if (hr != S_OK)
			{
				TCHAR tzBuf[MAX_STR];
				UStrPrint(tzBuf, TEXT("%s failed (0x%08X)"), spec.cmd.c_str(), (UINT) hr);
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

		// Append text to the current channel exactly as written: a finished
		// task's trail is copied back into the parent's log, and its lines
		// already carry a line number and a timestamp from the task's own clock,
		// so re-formatting them would stack a second header on each.
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

			SpawnAndTrace(L"proc.run", MakeProcSpec(path, args, 1), true, line, NULL, NULL);
			return MakeNil();
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
				DWORD dwWait = WaitForSingleObject(it->second, ProcTimeout(args, 1));
				DWORD dwCode = 0;

				if (dwWait == WAIT_TIMEOUT)
				{
					// Still running: keep the handle so a later await can retry.
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
			HRESULT hr = KillProc(name.c_str());

			// S_FALSE means "nothing by that name was running". The text command
			// reports that as its result rather than as an error, and a boot
			// chain relies on tolerating it, so it is traced, not raised.
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
			v.b = (FindProc((PCTSTR) name.c_str()) != 0);
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
				// a string.
				if ((v.kind == VK_INT) || (v.kind == VK_BOOL))
				{
					DWORD dw = (v.kind == VK_BOOL) ? (v.b ? 1u : 0u) : (DWORD) v.i;
					hr = RegSetValue(hRoot, subKey.c_str(), name.c_str(), REG_DWORD, &dw, sizeof(dw));
				}
				else
				{
					String text = ToDisplay(v);
					hr = RegSetValue(hRoot, subKey.c_str(), name.c_str(), REG_SZ,
						(const void *) text.c_str(), (DWORD) (text.size() * sizeof(TCHAR)));
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
			HKEY hRoot = NULL;
			String subKey;
			HRESULT hr;

			SplitRegKey(key, &hRoot, &subKey, line);
			hr = RegDeleteTree(hRoot, subKey.c_str());
			if (hr != S_OK)
			{
				throw Error(hr, String(L"reg.delete_tree failed for ") + key, line);
			}
			TraceVerb(String(L"reg.delete_tree ") + key, line, S_OK);
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

		Value HostFsList(Interp&, std::vector<Value>& args, int line)
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
					if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
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
			const BYTE *pb = data.empty() ? NULL : &data[0];
			size_t cb = data.size();
			int cch;
			UINT uCodePage;

			if ((cb >= 3) && (pb[0] == 0xEF) && (pb[1] == 0xBB) && (pb[2] == 0xBF))
			{
				pb += 3;
				cb -= 3;
			}
			else if ((cb >= 2) && (pb[0] == 0xFF) && (pb[1] == 0xFE))
			{
				return String((const wchar_t *) (pb + 2), (cb - 2) / sizeof(wchar_t));
			}
			if (cb == 0)
			{
				return String();
			}

			uCodePage = CP_UTF8;
			cch = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (LPCCH) pb, (int) cb, NULL, 0);
			if (cch <= 0)
			{
				uCodePage = CP_ACP;
				cch = MultiByteToWideChar(CP_ACP, 0, (LPCCH) pb, (int) cb, NULL, 0);
				if (cch <= 0)
				{
					return String();
				}
			}
			{
				String out((size_t) cch, L'\0');

				MultiByteToWideChar(uCodePage, 0, (LPCCH) pb, (int) cb, &out[0], cch);
				return out;
			}
		}

		// Text to file bytes, UTF-8 with a BOM on create. It round-trips through
		// fs.read, and it is what anything else on the machine -- notepad, a
		// .bat, another script -- reads back as the characters the script meant.
		HRESULT WriteTextFile(PCTSTR ptzPath, const String& text, BOOL bAppend)
		{
			std::vector<BYTE> bytes;
			int cb;

			if (!bAppend)
			{
				bytes.push_back(0xEF);
				bytes.push_back(0xBB);
				bytes.push_back(0xBF);
			}
			cb = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int) text.size(), NULL, 0, NULL, NULL);
			if (cb > 0)
			{
				size_t uAt = bytes.size();

				bytes.resize(uAt + (size_t) cb);
				WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int) text.size(),
					(LPSTR) &bytes[uAt], cb, NULL, NULL);
			}
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

		Value HostFsLines(Interp&, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"path");
			std::vector<BYTE> data;
			Value out;
			HRESULT hr = FsReadAll(path.c_str(), &data);

			if (hr != S_OK)
			{
				throw Error(hr, String(L"fs.lines: ") + path, line);
			}
			out.kind = VK_LIST;
			out.list = ListPtr(new std::vector<Value>());
			{
				std::vector<String> lines = SplitTextLines(DecodeFileText(data));

				for (size_t i = 0; i < lines.size(); i++)
				{
					out.list->push_back(MakeStr(lines[i]));
				}
			}
			TraceVerb(String(L"fs.lines ") + path, line, S_OK);
			return out;
		}

		Value HostFsWriteAppend(std::vector<Value>& args, int line, BOOL bAppend)
		{
			String path = ArgStr(args, 0, line, L"path");
			String text = (args.size() > 1) ? ToDisplay(args[1]) : String();
			HRESULT hr = WriteTextFile(path.c_str(), text, bAppend);

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

			// A directory that cannot be entered is not an error: the text form
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
			hr = WriteTextFile(path.c_str(), whole, FALSE);
			if (hr != S_OK)
			{
				throw Error(hr, String(L"fs.edit_line could not write ") + path, line);
			}
			TraceVerb(String(L"fs.edit_line ") + path, line, S_OK);
			return MakeNil();
		}

		// Attributes have no text command; walk it here. `recurse` mirrors the
		// "/S /D" of an "attrib" line: every subdirectory, and every file in it.
		bool SetAttrTree(const String& path, DWORD add, DWORD remove, bool recurse)
		{
			DWORD attr = GetFileAttributesW(path.c_str());

			if (attr == INVALID_FILE_ATTRIBUTES)
			{
				return false;
			}
			SetFileAttributesW(path.c_str(), (attr | add) & ~remove);

			if (recurse && (attr & FILE_ATTRIBUTE_DIRECTORY))
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
						SetAttrTree(path + L"\\" + fd.cFileName, add, remove, true);
					}
					while (FindNextFileW(hFind, &fd));
					FindClose(hFind);
				}
			}
			return true;
		}

		Value HostFsSetAttr(Interp&, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"path");
			DWORD add = 0;
			DWORD remove = 0;
			bool recurse = OptBool(args, 1, L"recurse", false);

			if (OptBool(args, 1, L"system", false))  { add |= FILE_ATTRIBUTE_SYSTEM; }
			if (OptBool(args, 1, L"hidden", false))  { add |= FILE_ATTRIBUTE_HIDDEN; }
			if (OptBool(args, 1, L"readonly", false)) { add |= FILE_ATTRIBUTE_READONLY; }
			if (OptBool(args, 1, L"clear_system", false)) { remove |= FILE_ATTRIBUTE_SYSTEM; }
			if (OptBool(args, 1, L"clear_hidden", false)) { remove |= FILE_ATTRIBUTE_HIDDEN; }

			if (!SetAttrTree(path, add, remove, recurse))
			{
				throw Error(ERROR_FILE_NOT_FOUND, L"fs.set_attr: no such path: " + path, line);
			}
			TraceVerb(L"fs.set_attr", line, S_OK);
			return MakeNil();
		}

		Value HostFsHide(Interp&, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"path");

			if (!SetAttrTree(path, FILE_ATTRIBUTE_HIDDEN, 0, false))
			{
				throw Error(ERROR_FILE_NOT_FOUND, L"fs.hide: no such path: " + path, line);
			}
			TraceVerb(L"fs.hide", line, S_OK);
			return MakeNil();
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

			// The icon index is its own argument, not a "#N" glued onto the file
			// name: the whole "shell32.dll#238" string would go to
			// SetIconLocation as if it were a file path, and produce no icon.
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

		Value HostShellHideWindow(Interp&, std::vector<Value>&, int line)
		{
			// The operation behind HIDE only ran on Windows 2000/XP, so on a
			// supported system it does nothing and reports success. The verb
			// stays -- scripts make the call -- and traces as a no-op.
			TraceVerb(L"shell.hide_window", line, S_OK);
			return MakeNil();
		}

		Value HostShellStart(Interp&, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"shell path");
			bool system = OptBool(args, 1, L"system", false);
			HRESULT hr;

			// { system: true } becomes the SHEL "-sys" switch, which the parser
			// recognises and then skips. It stays in the trace so the intent is
			// visible, but nothing acts on it.
			hr = SetSystemShell(path.c_str());
			if (hr != S_OK)
			{
				throw Error(hr, String(L"shell.start could not register ") + path, line);
			}
			TraceVerb(String(system ? L"shell.start -sys " : L"shell.start ") + path, line, S_OK);
			return MakeNil();
		}

		// Both of these ask the desktop shell (WinXShell.exe) to do the work: it
		// owns the taskbar and the folder-option dialog, so handing the request
		// to it is how a script pins a taskbar button or sets an option.
		Value HostShellPinTaskbar(Interp&, std::vector<Value>& args, int line)
		{
			String name = ArgStr(args, 0, line, L"program name");
			String exe = OptStr(args, 1, L"winxshell");
			ProcSpec spec;

			if (exe.empty())
			{
				exe = EnvValue(L"ProgramFiles") + L"\\WinXShell\\WinXShell.exe";
			}
			spec.cmd = QuoteAlways(exe) + L" -code " + QuoteAlways(L"PinToTaskbar([[" + name + L"]])");
			spec.hidden = true;
			SpawnAndTrace(L"shell.pin_to_taskbar", spec, false, line, NULL, NULL);
			return MakeNil();
		}

		Value HostDesktopFolderOption(Interp&, std::vector<Value>& args, int line)
		{
			String name = ArgStr(args, 0, line, L"option");
			String value = (args.size() > 1) ? ToDisplay(args[1]) : String(L"1");
			String exe = EnvValue(L"ProgramFiles") + L"\\WinXShell\\WinXShell.exe";
			ProcSpec spec;

			spec.cmd = QuoteAlways(exe) + L" -code " +
				QuoteAlways(L"FolderOptions:Set('" + name + L"', " + value + L")");
			spec.hidden = true;
			SpawnAndTrace(L"desktop.folder_option", spec, false, line, NULL, NULL);
			return MakeNil();
		}

		Value HostDesktopWallpaper(Interp&, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"path");
			HRESULT hr = SetWallpaper(path.c_str());

			if (hr != S_OK)
			{
				throw Error(ERROR_FILE_NOT_FOUND, L"desktop.set_wallpaper: no such image: " + path, line);
			}
			TraceVerb(String(L"desktop.set_wallpaper ") + path, line, S_OK);
			return MakeNil();
		}

		// --- hotkey ---------------------------------------------------------------

		// v1 takes the command text, not a closure: a hotkey fires later on the
		// message loop, where there is no interpreter frame left to call back
		// into. Documented as a divergence; the closure form is the design target.
		Value HostHotkeyBind(Interp&, std::vector<Value>& args, int line)
		{
			long long vk = ArgInt(args, 0, line, L"virtual key");
			String text;
			TCHAR tzBuf[64];

			if (args.size() < 2)
			{
				throw Error(0x80070057, L"hotkey.bind needs a command", line);
			}
			text = ToDisplay(args[1]);
			_snwprintf_s(tzBuf, _countof(tzBuf), _TRUNCATE, TEXT("HOTK #%I64d,"), vk);
			RunTextCommandOk(String(tzBuf) + text, line);
			return MakeNil();
		}

		// --- boot -----------------------------------------------------------------

		Value HostBootLogo(Interp&, std::vector<Value>& args, int line)
		{
			// boot.logo(img) shows that image; boot.logo() with no image -- which
			// is what boot.clear() maps to -- takes the splash down.
			if ((args.size() > 0) && (args[0].kind == VK_STR))
			{
				HRESULT hr = ShowLogo(args[0].s.c_str());

				if (hr != S_OK)
				{
					throw Error(hr, String(L"boot.logo failed for ") + args[0].s, line);
				}
				TraceVerb(String(L"boot.logo ") + args[0].s, line, S_OK);
			}
			else
			{
				ShowLogo(NULL);
				TraceVerb(L"boot.clear", line, S_OK);
			}
			return MakeNil();
		}

		Value HostBootText(Interp&, std::vector<Value>& args, int line)
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
			// the text markers mean by not naming them at all.
			hr = SetLogoText(text.c_str(), dwColor, iLeft, iTop, -1, -1, iSize, FALSE);
			if (hr != S_OK)
			{
				throw Error(hr, String(L"boot.text failed: ") + text, line);
			}
			TraceVerb(String(L"boot.text ") + text, line, S_OK);
			return MakeNil();
		}

		Value HostBootAddRun(Interp&, std::vector<Value>& args, int line)
		{
			String cmd = ArgStr(args, 0, line, L"command");
			String name = ArgStr(args, 1, line, L"name");
			HRESULT hr = SetStartupEntry(name.c_str(), cmd.c_str());

			if (hr != ERROR_SUCCESS)
			{
				throw Error((HRESULT) hr, String(L"boot.add_run failed for ") + name, line);
			}
			TraceVerb(String(L"boot.add_run ") + name, line, S_OK);
			return MakeNil();
		}

		Value HostBootRemoveRun(Interp&, std::vector<Value>& args, int line)
		{
			// Removing an entry that is not there leaves the caller where it
			// asked to be, so it is not an error -- same rule as reg.delete.
			String name = ArgStr(args, 0, line, L"name");
			HRESULT hr = RemoveStartupEntry(name.c_str());

			if (hr != S_OK)
			{
				throw Error(hr, String(L"boot.remove_run failed for ") + name, line);
			}
			TraceVerb(String(L"boot.remove_run ") + name, line, S_OK);
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
			// answering 1 is the answer. The text form spells the same thing
			// with the "positive codes are not failures" rule; here it is just
			// what the call returns.
			TraceVerb(String(L"native.call ") + dll + L"," + proc, line, S_OK);
			return MakeInt((long long) lResult);
		}

		Value HostNativeLoad(Interp&, std::vector<Value>& args, int line)
		{
			// "CALL $dll" with no comma calls DllRegisterServer, which is what
			// an Initapp line relies on for FastEx64.dll.
			//
			// A library that will not load is not raised: the text form answers
			// success there, and a boot step should not stop the chain because
			// an optional DLL is absent.
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

			spec.cmd = QuoteAlways(L"drvload.exe") + L" " + Quote(inf);
			spec.hidden = true;
			SpawnAndTrace(L"drv.load", spec, wait, line,
				wait ? NULL : &hProcess, wait ? NULL : &dwPid);
			if (wait)
			{
				return MakeNil();
			}
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


		Value HostSvcStart(Interp&, std::vector<Value>& args, int line)
		{
			String name = ArgStr(args, 0, line, L"service name");
			// "net start", which this replaced, returns once the service is
			// RUNNING; the text form only does that with -wait. Default false
			// so a caller can still ask for fire-and-forget.
			bool wait = OptBool(args, 1, L"wait", false);
			HRESULT hr = SetServiceState(name.c_str(), FALSE, wait ? TRUE : FALSE);

			// Already running is the state the caller asked for, so it is not an
			// error here. The text form's SERV reports 1056 in that case, but
			// this verb is a postcondition, not a report.
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

			v = Field(args, 2, L"type");
			if (v && (v->kind == VK_INT)) { dwType = (DWORD) v->i; }
			v = Field(args, 2, L"start");
			if (v && (v->kind == VK_INT)) { dwStart = (DWORD) v->i; }
			v = Field(args, 2, L"error");
			if (v && (v->kind == VK_INT)) { dwError = (DWORD) v->i; }

			hr = CreateServiceEntry(name.c_str(), path.c_str(),
				display.empty() ? NULL : display.c_str(),
				dwType, dwStart, dwError, deps.empty() ? NULL : deps.c_str());
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

			SetEnvironmentVariableW(name.c_str(), value.c_str());
			TraceVerb(L"env.set", line, S_OK);
			return MakeNil();
		}

		// A real wait, unlike `timeout /t`: that one needs a console and returns
		// at once under a PE launch, so its "1 second" never happens.
		Value HostTimeSleep(Interp&, std::vector<Value>& args, int line)
		{
			long long ms = ArgInt(args, 0, line, L"milliseconds");

			// TimeDelay is the capability the text-form WAIT has always used, and it
			// keeps the message pump alive while it waits, so a splash or a tray
			// icon does not freeze. A bare Sleep would be a different operation.
			//
			// The text form answers anything <= 0 by waiting for a keypress
			// instead; that is a quirk of the text form, not something a caller
			// asking for a number of milliseconds can mean.
			if (ms > 0)
			{
				TimeDelay((DWORD) ms);
			}
			TraceVerb(String(L"time.sleep ") + ToText(MakeInt(ms)), line, S_OK);
			return MakeNil();
		}

		// --- sys ------------------------------------------------------------------

		// Boot() is the capability the desktop context menu already reaches
		// through the text-form SHUT command: it broadcasts the end-session
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

			if (hr != S_OK)
			{
				throw Error(hr, String(L"sys.recycle_capacity: no volume matched ") + drive, line);
			}
			TraceVerb(String(L"sys.recycle_capacity ") + drive, line, S_OK);
			return MakeNil();
		}

		Value HostSysOwner(Interp&, std::vector<Value>& args, int line)
		{
			String owner = ArgStr(args, 0, line, L"owner");
			String org = (args.size() > 1) ? ToDisplay(args[1]) : String();

			// The text form writes both values from one command; the language
			// says which is which instead of splitting on a comma.
			SetOwnerInfo(owner.c_str(), org.c_str());
			TraceVerb(String(L"sys.owner ") + owner, line, S_OK);
			return MakeNil();
		}

		// The two halves of INIT that the text form's "I" and "K" letters carried.
		// Each one records a request and nothing more: "I" says the tray icon is
		// wanted, "K" says the low-level keyboard hook is wanted, and the shell's
		// own startup reads the flag back and does the drawing or the installing.
		// So the verb writes the same registry DWORD to the same place the
		// text-form INIT writes it, and the shell reads it back from there.
		Value HostSysTrayIcon(Interp&, std::vector<Value>&, int line)
		{
			BOOL wanted = TRUE;
			HRESULT hr = RegSetValue(HKEY_LOCAL_MACHINE, PELOGON, TEXT("TrayIcon"),
				REG_DWORD, &wanted, sizeof(wanted));

			if (hr != S_OK)
			{
				throw Error(hr, L"sys.tray_icon: the flag could not be written", line);
			}
			TraceVerb(L"sys.tray_icon", line, S_OK);
			return MakeNil();
		}

		Value HostSysHotkeyHook(Interp&, std::vector<Value>&, int line)
		{
			BOOL wanted = TRUE;
			HRESULT hr = RegSetValue(HKEY_LOCAL_MACHINE, PELOGON, TEXT("InitProc"),
				REG_DWORD, &wanted, sizeof(wanted));

			if (hr != S_OK)
			{
				throw Error(hr, L"sys.hotkey_hook: the flag could not be written", line);
			}
			TraceVerb(L"sys.hotkey_hook", line, S_OK);
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
			// an absent option means -- the text-form "P0" is mapped onto 1 by
			// GetNewSize before it reaches the same arithmetic.
			v = Field(args, 0, L"percent");
			if (v && (v->kind == VK_INT) && (v->i > 0)) { dwPercent = (DWORD) v->i; }
			v = Field(args, 0, L"min_mb");
			if (v && (v->kind == VK_INT) && (v->i > 0)) { dwMinMb = (DWORD) v->i; }
			v = Field(args, 0, L"max_mb");
			if (v && (v->kind == VK_INT) && (v->i > 0)) { dwMaxMb = (DWORD) v->i; }

			dwSize = FbwfCacheSize(dwPercent, dwMinMb, dwMaxMb, MD5Str_Len, 1024, 32);
			hr = ApplyFbwfCache(dwSize);
			// Already running is where the caller wants the service, and FBWF is
			// running in this PE by the time any script can ask for it. The
			// text form keeps reporting 1056 there; this is a postcondition.
			if ((hr != S_OK) && (hr != (HRESULT) ERROR_SERVICE_ALREADY_RUNNING))
			{
				throw Error(hr, L"sys.fbwf could not start the FBWF service", line);
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

			// The text form only writes one when the machine needs it: free
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
			// purpose, because the text form produces both.
			hr = EjectDrives((drive.empty() || (drive == L"*")) ? NULL : drive.c_str(),
				(UINT) retry, bCDROM, bUSB);
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
			// The names it sets are the text form's -- CDROM, CDROM0 (the count)
			// and CDROM1.. -- and reading those back is what a script does.
			TraceVerb(String(L"disk.publish_letters ") + kind + L" n=" + ToText(MakeInt((long long) n)), line, S_OK);
			return MakeInt((long long) n);
		}

		Value HostDiskMountVolumes(Interp&, std::vector<Value>& args, int line)
		{
			String from = OptStr(args, 0, L"from");
			TCHAR cFrom = from.empty() ? TEXT('C') : from[0];
			INT n = MountUnmountedVolumes(cFrom);

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

		Value HostDiskRamDisk(Interp&, std::vector<Value>& args, int line)
		{
			const Value *v = Field(args, 0, L"size_mb");
			DWORD dwWanted = 0;
			DWORD dwSizeMb;
			HRESULT hr;

			if (v && (v->kind == VK_INT) && (v->i > 0)) { dwWanted = (DWORD) v->i; }
			// The same clamping the text form reaches through GetNewSize:
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
		// call rather than a variable. The text form had %RANDOM%, a built-in
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

		Value HostRandInt(Interp&, std::vector<Value>& args, int line)
		{
			long long n = ArgInt(args, 0, line, L"bound");
			long long drawn;

			if (n <= 0)
			{
				throw Error(0x80070057, L"rand.int: the bound must be positive", line);
			}
			drawn = (long long) (NextRandom() % (ULONGLONG) n);
			TraceVerb(String(L"rand.int ") + ToText(MakeInt(drawn))
				+ L" of " + ToText(MakeInt(n)), line, S_OK);
			return MakeInt(drawn);
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
		// program. The shape this exists for is "drop a file in a folder and let it
		// run beside the others", which is why the task gets its own trail instead
		// of sharing the parent's.
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

		Value HostRun(Interp& in, std::vector<Value>& args, int line)
		{
			String path = ArgStr(args, 0, line, L"script path");
			bool wait = OptBool(args, 1, L"wait", true);
			HRESULT hr;

			if (!UFileExist((PTSTR) path.c_str()))
			{
				throw Error(ERROR_FILE_NOT_FOUND, L"run: no such script: " + path, line);
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
		// Give the volume on a given bus a drive letter. AssignLetterToBus
		// documents what the result does and does not change.
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
			{ L"log.open",             HostLogOpen },
			{ L"log.close",            HostLogClose },
			{ L"log.info",             HostLogInfo },
			{ L"log.warn",             HostLogWarn },
			{ L"log.error",            HostLogError },
			{ L"debug.desktop",        HostDebugDesktop },
			{ L"proc.run",             HostProcRun },
			{ L"proc.spawn",           HostProcSpawn },
			{ L"proc.kill",            HostProcKill },
			{ L"proc.exists",          HostProcExists },
			{ L"proc.await",           HostProcAwait },
			{ L"proc.await_all",       HostProcAwaitAll },
			{ L"reg.set",              HostRegSet },
			{ L"reg.delete",           HostRegDelete },
			{ L"reg.delete_tree",      HostRegDeleteTree },
			{ L"reg.get",              HostRegGet },
			{ L"rand.int",             HostRandInt },
			{ L"rand.range",           HostRandRange },
			{ L"rand.pick",            HostRandPick },
			{ L"rand.seed",            HostRandSeed },
			{ L"reg.load_hive",        HostRegLoadHive },
			{ L"reg.unload_hive",      HostRegUnloadHive },
			{ L"fs.exists",            HostFsExists },
			{ L"fs.list",              HostFsList },
			{ L"fs.delete",            HostFsDelete },
			{ L"fs.mkdir",             HostFsMkdir },
			{ L"fs.read",              HostFsRead },
			{ L"fs.lines",             HostFsLines },
			{ L"fs.write",             HostFsWrite },
			{ L"fs.append",            HostFsAppend },
			{ L"fs.rename",            HostFsRename },
			{ L"fs.hardlink",          HostFsHardlink },
			{ L"fs.temp_dir",          HostFsTempDir },
			{ L"fs.chdir",             HostFsChdir },
			{ L"fs.clean_temp",        HostFsCleanTemp },
			{ L"fs.edit_line",         HostFsEditLine },
			{ L"fs.set_attr",          HostFsSetAttr },
			{ L"fs.hide",              HostFsHide },
			{ L"shell.shortcut",       HostShellShortcut },
			{ L"shell.hide_window",    HostShellHideWindow },
			{ L"shell.start",          HostShellStart },
			{ L"shell.pin_to_taskbar", HostShellPinTaskbar },
			{ L"desktop.folder_option", HostDesktopFolderOption },
			{ L"desktop.set_wallpaper", HostDesktopWallpaper },
			{ L"hotkey.bind",          HostHotkeyBind },
			{ L"boot.logo",            HostBootLogo },
			{ L"boot.text",            HostBootText },
			{ L"boot.add_run",         HostBootAddRun },
			{ L"boot.remove_run",      HostBootRemoveRun },
			{ L"boot.clear",           HostBootLogo },
			{ L"native.call",          HostNativeCall },
			{ L"native.load",          HostNativeLoad },
			{ L"drv.load",             HostDrvLoad },
			{ L"svc.start",            HostSvcStart },
			{ L"svc.stop",             HostSvcStop },
			{ L"svc.query",            HostSvcQuery },
			{ L"svc.create",           HostSvcCreate },
			{ L"svc.delete",           HostSvcDelete },
			{ L"disk.letter",          HostDiskLetter },
			{ L"disk.eject",           HostDiskEject },
			{ L"disk.mount_volumes",   HostDiskMountVolumes },
			{ L"disk.publish_letters", HostDiskPublishLetters },
			{ L"disk.mount_image",     HostDiskMountImage },
			{ L"disk.ram_disk",        HostDiskRamDisk },
			{ L"env.get",              HostEnvGet },
			{ L"env.set",              HostEnvSet },
			{ L"time.sleep",           HostTimeSleep },
			{ L"sys.shutdown",         HostSysShutdown },
			{ L"sys.reboot",           HostSysReboot },
			{ L"sys.recycle_capacity", HostSysRecycle },
			{ L"sys.owner",            HostSysOwner },
			{ L"sys.tray_icon",        HostSysTrayIcon },
			{ L"sys.hotkey_hook",      HostSysHotkeyHook },
			{ L"sys.fbwf",             HostSysFbwf },
			{ L"sys.page_file",        HostSysPageFile },
			{ L"run",                  HostRun },
			{ NULL, NULL }
		};
	}	// namespace

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

	Value CallHost(Interp& in, const String& name, std::vector<Value>& args, int line)
	{
		const HostEntry *e = FindHost(name);

		if (!e)
		{
			throw Error(0x80070002, String(L"unknown host function '") + name + L"'", line);
		}
		if (in.dryRun && IsWriteVerb(name))
		{
			TraceVerb(String(L"(dry-run) ") + name, line, S_OK);
			return MakeNil();
		}
		// A "begin" line before the call and the command's own line after it.
		// Without the first one a boot that stops mid-call looks exactly like a
		// boot that finished - there is simply no further trace.
		TraceVerb(String(L"begin ") + name, line, S_OK);
		try
		{
			return e->fn(in, args, line);
		}
		catch (Error& err)
		{
			if (IsOptional(args))
			{
				TraceVerb(name + L" (optional) " + BriefArg(args), line, (HRESULT) err.code);
				return MakeNil();
			}
			throw;
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
	}
}
