#pragma once

#include "base.h"

// CP_ACP <-> UTF-16. The code page is CP_ACP on purpose: the scripts and the
// configured values this project reads are ANSI/GBK, so the bytes are taken as
// whatever the current system says they are.
//
// Both calls take a NUL-terminated source and a destination capacity counted in
// characters, and both return the destination.

PWSTR UAStrToWStr(PWSTR pwzDst, PCSTR pszSrc, UINT uLen);
PSTR UWStrToAStr(PSTR pszDst, PCWSTR pwzSrc, UINT uLen);