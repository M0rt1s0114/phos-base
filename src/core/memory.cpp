#include "memory.h"

namespace
{
	// Physical memory in megabytes, or 0 if the query fails.
	//
	// The shift happens on the 64-bit value and only the result is narrowed. The
	// earlier form narrowed first, so on a machine with 4 GB or more the high
	// bits were thrown away before the division -- 8 GB read as 0.
	DWORD MemoryInMegaBytes(BOOL bFree)
	{
		MEMORYSTATUSEX statex;

		ZeroMemory(&statex, sizeof(statex));
		statex.dwLength = sizeof(statex);
		if (!GlobalMemoryStatusEx(&statex))
		{
			return 0;
		}
		return (DWORD) ((bFree ? statex.ullAvailPhys : statex.ullTotalPhys) >> 20);
	}
}

DWORD GetFreeMemo(VOID)
{
	return MemoryInMegaBytes(TRUE);
}

DWORD GetTotalMemo(VOID)
{
	return MemoryInMegaBytes(FALSE);
}