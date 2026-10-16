// Script loading.
//
// This file used to be the dialect's engine: a line scanner that cut each line at
// a backtick, _SUB/_END subprocesses, CALL, TEAM, FORX, and the encoding sniffing
// that went with them. All of that is gone.
//
// What is left is the way into the language front end in src/lang/: LoadFile reads
// a file and hands its bytes over, and Load is what the boot entry calls. A file
// that does not carry the marker is refused here rather than parsed as anything
// else, because the two formats have nothing in common and the failure of reading
// one as the other is a boot that stops for a reason nobody can see.

// Defined beside the rest of the command line, at the end of the unity build:
// what it needs is where this executable is.
VOID ReportStartupError(PCTSTR ptzMessage);
HRESULT LoadFile(PTSTR ptzPath)
{	
	// MAX_PATH, not MAX_NAME, and a bounded copy. UStrCopy is an unbounded
	// wcscpy, and MAX_NAME is 80: a script path longer than 79 characters ran
	// off the end of this buffer. The damage did not look like a crash either -
	// it showed up as an unrelated window and a process that never exited.
	TCHAR tzPath[MAX_PATH];
	UStrCopyN(tzPath, ptzPath, _NumOf(tzPath));

	UINT uSize = (UINT) -1;
	PBYTE pbFile = (PBYTE) UFileLoad(tzPath, &uSize);
	if (pbFile == NULL)
	{
		return S_FALSE;
	}

	// Go into the directory the script came from, and publish its drive letter as
	// CurDrv, which scripts read.
	//
	// The path itself is left whole. It goes on to the language as the script's
	// own path -- what `script.file` answers, and what a crash record names -- and
	// this used to shorten it to "X:" in place, so both of those were wrong
	// (docs/FINDINGS-NEW.md N-01).
	//
	// The move is scoped to this call. A script's own directory is what a relative
	// path inside it should mean; leaving the process there afterwards moves the
	// *caller's* idea of "here", so the parent's next relative write lands in the
	// child's folder -- silently, which is what makes it worth a case
	// (cases-lang/053-script-cwd-scope.phos). The guard restores on every way out.
	TCHAR tzPrevious[MAX_PATH] = {0};
	BOOL bRestoreDir = FALSE;

	struct RestoreDir
	{
		PTSTR ptzDir;
		BOOL *pbRestore;
		~RestoreDir()
		{
			if (*pbRestore)
			{
				SetCurrentDirectory(ptzDir);
			}
		}
	} restore = { tzPrevious, &bRestoreDir };

	{
		PTSTR ptzSlash = UStrRChr(tzPath, '\\');

		if (ptzSlash)
		{
			TCHAR tzDir[MAX_PATH] = {0};

			// The separator is part of the directory, so one more character than
			// sits in front of it, plus the terminator.
			UStrCopyN(tzDir, tzPath, (UINT) (ptzSlash - tzPath) + 2);

			// Where to put it back to. A directory that cannot be read leaves
			// nothing to restore, and the script runs where it already is.
			if (GetCurrentDirectory(_NumOf(tzPrevious), tzPrevious) > 0)
			{
				bRestoreDir = TRUE;
			}
			SetCurrentDirectory(tzDir);

			// "X:" only means something when the path really has a drive on it;
			// a relative name has two characters of its own there.
			if (tzPath[1] == ':')
			{
				TCHAR tzDrive[3] = {0};

				tzDrive[0] = tzPath[0];
				tzDrive[1] = tzPath[1];
				SetEnvironmentVariable(TEXT("CurDrv"), tzDrive);
			}
		}
	}

	// A script says what it is on its first line. A file without the marker is
	// what the text dialect used to be read from -- a PECMD .ini, an old copy of
	// the boot chain, a .wcs -- and there is nothing to do with it but say so.
	//
	// Refused by name rather than guessed at, and the notice is durable: the
	// caller may be the boot entry, which has nowhere to print an error and no
	// trace channel open this early. A boot that stops because the registry
	// still names an .ini has to leave a reason behind.
	if (!Lang::IsMarker(pbFile, uSize))
	{
		TCHAR tzMessage[MAX_STR];

		UStrPrint(tzMessage,
			TEXT("phos: not a script: %s\n")
			TEXT("a script starts with #!phos, and the PECMD text dialect "
				"(the .ini files it read) was removed.\n")
			TEXT("See the migration notes in this project's Wiki.\n"),
			tzPath);
		ReportStartupError(tzMessage);
		UMemFree(pbFile);
		return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
	}

	{
		HRESULT hrStructured = Lang::RunBytes(pbFile, uSize, tzPath);

		UMemFree(pbFile);

		// A script that stops after it started leaves a durable reason.
		//
		// RunBytes records the line and the message on the trace channel, and
		// the boot entry normally has no channel open: PHOS_DEBUG_LOG is what
		// opens one, and a shipped image does not set it. So a parse error, a
		// verb that does not resolve, or a runtime error before the script's own
		// `log.open` used to leave *nothing* -- the machine stopped and the disk
		// said nothing about why, which is the failure this whole file exists to
		// prevent (the no-marker branch above already leaves a note for its own
		// case).
		//
		// Written even when a channel is open: the file is the copy that
		// survives the reboot, and it names the script rather than just the line.
		if (hrStructured != S_OK)
		{
			TCHAR tzMessage[MAX_STR];

			UStrPrint(tzMessage,
				TEXT("phos: %s stopped (0x%08X)\n")
				TEXT("`phos check <script>` names the line and the reason.\n"),
				tzPath, (UINT) hrStructured);
			ReportStartupError(tzMessage);
		}
		return hrStructured;
	}
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
		// A path with no drive on it used to be searched for -- and *run* -- on
		// every drive where it existed, one after another, and the caller was
		// never told which copy won. That is the shape a planted file exploits:
		// the boot chain runs with the rights to do anything to the machine, and
		// a USB stick that happens to be plugged in is enumerated in the same
		// list as the ramdisk. Audit S-01.
		//
		// Refused rather than narrowed to "the first one found", because
		// narrowing it would still be a search and the caller still could not see
		// which copy it got. The replacement is explicit: fs.find_on_drives(rel)
		// lists where a relative path exists, the script names the one it wants,
		// and `run` takes one concrete path. See SPEC-10 in
		// docs/SPEC-DECISIONS.md.
		pFileStart = ptzSavedFileStart;
		return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
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
	// the shipped boot chain depends on that.
	//
	// Measured against the image's own Pecmd.exe: its MAIN hands a script
	// %Desktop% already expanded. On the official 240512 build the same is true
	// only because that boot chain runs INIT (which is where this setup lives
	// here), and the current HTBPE has no INIT anywhere - while Post\Link.ini is
	// a pile of "LINK %Desktop%\..." lines. Without this they are written into a
	// directory literally called "%Desktop%", and the command still reports
	// success.
	{
		TCHAR tzEnv[MAX_NAME];
		tzEnv[0] = 0;
		SetEnvironment(tzEnv, FALSE, TRUE);
	}

	// The state key carries a schema number. Nothing reads it yet: it is the
	// cheap half of letting a later version tell "an image that an older build
	// wrote this key into" from "the key is there and means what this version
	// says". See docs/SPEC-DECISIONS.md, SPEC-11.
	{
		DWORD dwSchema = 1;

		SHSetValue(HKEY_LOCAL_MACHINE, PHOS_STATE, TEXT("SchemaVersion"), REG_DWORD,
			&dwSchema, sizeof(dwSchema));
	}

	// Switch desktop - and record what happened, because every one of these
	// three calls can fail and none of them says so. This runs in the boot
	// driver, before any script has opened a trace channel, so the result goes
	// to the registry where the test harness picks it up.
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
		SHSetValue(HKEY_LOCAL_MACHINE, PHOS_STATE, TEXT("DesktopProbe"), REG_SZ,
			tzProbe, (UStrLen(tzProbe) + 1) * sizeof(TCHAR));
	}
	// The window and the shell watcher come up first, and the script runs *here*
	// rather than in a child.
	//
	// It used to be the other way round: MAIN spawned "<self> LOAD <script>" and
	// went straight to the message loop. That puts the chain and the window in
	// two different processes, which is what makes a hotkey binding impossible to
	// keep in memory -- the binding is made in the child and fires in the parent,
	// so the only thing that can cross is text, parked in the registry and parsed
	// again on the other side. The injection surface S-02 closed and the round
	// trip SPEC-11 says disappears both live in that gap.
	//
	// The order is the part that matters: MainThread has to be watching for the
	// shell *before* the chain starts it, or it sees a shell that is already up
	// and leaves without ever keeping it alive -- which is the whole job.
	MainWindowHwnd = CreateMainWindow(g_hInst/*, SW_HIDE*/);

	UThreadClose(UThreadCreate((UPROC) MainThread, NULL));

	// Execute the boot script. Going through Load rather than LoadFile keeps the
	// refusal to search every drive for a path with no drive on it (audit S-01).
	if (ptzCmd[0])
	{
		Load(g_ctx, ptzCmd);
	}

	SetProcessWorkingSetSize(CurrentProcess, (SIZE_T) -1, (SIZE_T) -1);

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
