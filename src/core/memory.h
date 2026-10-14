#pragma once

#include "base.h"

// Physical memory, in whole megabytes. 0 means the query failed, which for these
// two is the same answer as "none", so callers do not have to branch on it.

DWORD GetFreeMemo(VOID);
DWORD GetTotalMemo(VOID);