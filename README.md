# Phosphor Base

[中文说明](README_CN.md) | **English**

A startup commander for Windows PE: one executable that runs a boot configuration script and provides the operations a PE boot needs.

[![License](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%20PE-lightgrey.svg)](#requirements)

## Table of Contents

- [Overview](#overview)
- [Status](#status)
- [Requirements](#requirements)
- [Install](#install)
- [Usage](#usage)
- [Building](#building)
- [Repository layout](#repository-layout)
- [Not included](#not-included)
- [Contributing](#contributing)
- [Acknowledgements](#acknowledgements)
- [License](#license)

## Overview

`phos.exe` runs a script while a Windows PE image boots. A script can be written in either of two forms:

- **Command files** (`.ini`): one command per line. The command set is documented in this repository's Wiki (in progress).
- **Structured scripts** (`.phos`, shebang `#!phos`): statements, blocks, functions, variables and error handling. Its library calls into the same operations as the commands.

The two forms can be mixed: a structured script can run a command line, and a command file can load a structured script.

Operations available to a boot configuration:

| Area | Operations |
|---|---|
| Services | start, stop, query, create, delete |
| Volumes | assign drive letters to volumes that have none; eject removable media and CD-ROMs |
| Images | mount a WIM image to a directory; unmount it; set the Ramdriv ramdisk size |
| Recycle bin | set the capacity per volume |
| Files | copy, delete, create directories, read, write, hard links, shortcuts, working directory |
| Attributes | file attributes and timestamps |
| Registry | read, write and delete keys and values; load and unload offline hives |
| Processes | run a program (hidden, on a chosen desktop, waiting or not); call a DLL entry point; send a message; wait; kill |
| Environment | machine, user and process environment; PE variable scopes |
| Display | display mode, page file, write-filter cache size, NumLock |
| Boot screen | splash image, splash text, wallpaper, fonts |
| Shell | hand over to a shell and restart it if it exits; tray icon and menu; global hotkeys; keyboard hook |
| Dialogs | message boxes; folder picker |
| Text and numbers | substring, position, replace, size, name, extension, directory, dates, base64, hash |

## Status

- Version: `0.1.0-beta1`
- Build targets: x64 and x86 (Win32).
- Verification: the boot chain and a command test matrix are executed in a Windows PE image on VMware. That environment is not part of this repository.

## Requirements

- **To run:** a Windows PE image (Windows 10 or later based). The program calls Windows APIs that exist in the PE environment; it is not intended for a full desktop installation.
- **To build:** Visual Studio 2019 or 2022 with the C++ workload (toolset `v142`) and a Windows 10 SDK. No third-party libraries; the system libraries used are `ShLwApi`, `Version` and `SetupAPI`.

## Install

There is no installer. Copy `phos.exe` into the image's `\Windows\System32\`.

A script named `init.phos` in the same directory is the one `--init` runs (see below).

## Usage

### Boot entry

Set the image's boot command:

    HKLM\SYSTEM\Setup\CmdLine = phos.exe --init

`--init` runs `init.phos` from the directory of `phos.exe`. A different file can be named: `phos.exe --init D:\boot\start.phos`. The command line contains no variable and no script path, so it does not depend on the drive letter the image boots from, and the process stays alive after the script ends to keep the shell running.

### Script host

    phos.exe LOAD  X:\Windows\System32\disposition\LoginUser.phos
    phos.exe FILE  X:\temp\*.*
    phos.exe run   hello.phos
    phos.exe check hello.phos
    phos.exe version

- `LOAD <file>` runs a file, in either form.
- `<COMMAND> <arguments>` runs a single command.
- `run <file>` executes a structured script.
- `check <file> [report]` parses a script and writes the verdict to `report` (default: `<file>.check.txt`), without executing it.
- `version` prints the version.

Without arguments, `phos.exe` shows a dialog with the version and the project address.

### Logging

A PE has no console. The trace channel writes every command and its result as it runs:

    set PHOS_DEBUG_LOG=\\.\COM1

The target is a file name or a device path. With the variable unset, no trace is written.

## Building

    msbuild phosphor.vcxproj /p:Configuration=Release /p:Platform=x64
    msbuild phosphor.vcxproj /p:Configuration=Release /p:Platform=Win32

Output: `build\x64\phos.exe`, `build\Win32\phos.exe`.

The sources are compiled as one translation unit: `src/app/unity.cpp` includes the area files, and that include order is the declaration order of the program. A new source file is added to the build by adding its include there.

## Repository layout

    src/app         entry point, command line, global state, resources, the unity include
    src/core        strings, paths, encoding, hashing, memory queries, disk queries
    src/engine      command dispatcher; parser of the command files
    src/commands    command implementations
    src/platform    process, registry, volume, display, tray and window operations
    src/lang        the structured script language: lexer, parser, interpreter, library

## Not included

- The WinPE image. The image is assembled separately.
- The test environment. It runs virtual machines and contains image files.
- The user interface shell. The tray icon and the message window in this repository are the minimum the boot chain needs; a separate shell is a separate project.
- Documentation for the language. It is written in this repository's Wiki (work in progress).

## Contributing

Bug reports and pull requests are handled in this repository's issue tracker. The command set and the script syntax are a compatibility surface: additions are accepted, changes that alter the result of an existing command or script are not.

## Acknowledgements

- Thanks to [@Kazusa1085](https://github.com/Kazusa1085) for the automated WinPE test bench.
- Thanks to [PECMD 2.8.8U](https://github.com/grub4dos/pecmd) and its contributors for inspiring this project.

## License

GPL-3.0 (see [LICENSE](LICENSE)). Copyright (C) 2026 Mortis0114 & Kazusa1085.
