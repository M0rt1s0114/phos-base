#pragma once

#include "base.h"

// The string and path helpers every other module leans on. All five work on the
// caller's buffer, in place, and none of them allocates.

PTSTR UStrLeftTrim(PTSTR ptstr);
PTSTR UStrRightTrim(PTSTR ptstr);
PTSTR UStrTrim(PTSTR ptstr);
PTSTR UPathSplitName(PTSTR ptzPath);
PTSTR USplitStr(PTSTR ptzStr);