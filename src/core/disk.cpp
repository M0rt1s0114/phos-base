#include "disk.h"

namespace
{
	// One query, two answers. GetDiskFreeSpaceEx wants three output pointers and
	// two of them are very rarely used, so this keeps the interesting parts.
	BOOL QueryVolume(PCTSTR pszDrive, ULONGLONG *puFree, ULONGLONG *puTotal)
	{
		ULARGE_INTEGER uFree;
		ULARGE_INTEGER uTotal;

		uFree.QuadPart = 0;
		uTotal.QuadPart = 0;
		if (!pszDrive || !pszDrive[0])
		{
			return FALSE;
		}
		if (!GetDiskFreeSpaceEx(pszDrive, NULL, &uTotal, &uFree))
		{
			return FALSE;
		}
		if (puFree)
		{
			*puFree = uFree.QuadPart;
		}
		if (puTotal)
		{
			*puTotal = uTotal.QuadPart;
		}
		return TRUE;
	}
}

DWORD GetDiskFree(PCTSTR pszDrive)
{
	ULONGLONG uFree;

	return QueryVolume(pszDrive, &uFree, NULL) ? (DWORD) (uFree >> 20) : 0;
}

DWORD GetDiskTotal(PCTSTR pszDrive)
{
	ULONGLONG uTotal;

	return QueryVolume(pszDrive, NULL, &uTotal) ? (DWORD) (uTotal >> 20) : 0;
}

BOOL DiskIsExist(PCTSTR pszDrive)
{
	return QueryVolume(pszDrive, NULL, NULL);
}