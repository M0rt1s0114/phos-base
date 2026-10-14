#pragma once

#include "base.h"

// Volume sizes, in whole megabytes. A size of 0 means the volume could not be
// queried -- which covers "not there", "no media" and "not ready" alike.
//
// The query's failure is checked: a caller comparing sizes must not be handed
// whatever happened to be on the stack.

DWORD GetDiskFree(PCTSTR pszDrive);
DWORD GetDiskTotal(PCTSTR pszDrive);

// Whether the volume answers at all. This is the same question the size calls
// ask, so it is the same failure that makes them return 0.
BOOL DiskIsExist(PCTSTR pszDrive);
