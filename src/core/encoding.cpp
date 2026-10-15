#include "encoding.h"

PWSTR UAStrToWStr(PWSTR pwzDst, PCSTR pszSrc, UINT uLen)
{
	if (!pwzDst || !pszSrc || !uLen)
	{
		return pwzDst;
	}
	// A conversion that does not fit answers zero and leaves the buffer
	// undefined, and the callers read what comes back as a string: hand them an
	// empty one rather than whatever was on the stack.
	if (MultiByteToWideChar(CP_ACP, 0, pszSrc, -1, pwzDst, (int) uLen) == 0)
	{
		pwzDst[0] = 0;
	}
	pwzDst[uLen - 1] = 0;
	return pwzDst;
}

PSTR UWStrToAStr(PSTR pszDst, PCWSTR pwzSrc, UINT uLen)
{
	if (!pszDst || !pwzSrc || !uLen)
	{
		return pszDst;
	}
	if (WideCharToMultiByte(CP_ACP, 0, pwzSrc, -1, pszDst, (int) uLen, NULL, NULL) == 0)
	{
		pszDst[0] = 0;
	}
	pszDst[uLen - 1] = 0;
	return pszDst;
}
