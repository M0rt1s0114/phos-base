#include "encoding.h"

PWSTR UAStrToWStr(PWSTR pwzDst, PCSTR pszSrc, UINT uLen)
{
	if (!pwzDst || !pszSrc || !uLen)
	{
		return pwzDst;
	}
	MultiByteToWideChar(CP_ACP, 0, pszSrc, -1, pwzDst, (int) uLen);
	return pwzDst;
}

PSTR UWStrToAStr(PSTR pszDst, PCWSTR pwzSrc, UINT uLen)
{
	if (!pszDst || !pwzSrc || !uLen)
	{
		return pszDst;
	}
	WideCharToMultiByte(CP_ACP, 0, pwzSrc, -1, pszDst, (int) uLen, NULL, NULL);
	return pszDst;
}