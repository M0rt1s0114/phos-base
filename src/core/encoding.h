#pragma once

#include "base.h"
#include <string>

// CP_ACP <-> UTF-16. The code page is CP_ACP on purpose: the scripts and the
// configured values this project reads are ANSI/GBK, so the bytes are taken as
// whatever the current system says they are.
//
// Both calls take a NUL-terminated source and a destination capacity counted in
// characters, and both return the destination.

PWSTR UAStrToWStr(PWSTR pwzDst, PCSTR pszSrc, UINT uLen);
PSTR UWStrToAStr(PSTR pszDst, PCWSTR pwzSrc, UINT uLen);

// Bytes to text, one rule for the whole program.
//
// A byte-order mark decides when there is one; with none, valid UTF-8 is UTF-8
// and anything else is the ANSI code page. The rule matters because one PE
// holds both kinds side by side with no mark on either, and guessing wrong does
// not look broken -- it produces different Chinese characters, or a path that
// does not exist.
//
// There were two copies of this (the script loader and the file reader) and
// they had already drifted: only one of them understood a UTF-16 byte-order
// mark. Deliberately no replacement characters: an ill-formed sequence under
// UTF-8 falls back to the ANSI code page rather than turning into U+FFFD.
std::wstring DecodeTextBytes(const BYTE *pb, size_t cb);
