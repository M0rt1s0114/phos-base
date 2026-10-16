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

std::wstring DecodeTextBytes(const BYTE *pb, size_t cb)
{
	std::wstring out;

	if (!pb || (cb == 0))
	{
		return out;
	}

	// A byte-order mark decides, and it is the only thing that does: a PE holds
	// UTF-8 and ANSI scripts side by side with no mark on either, so the
	// fallback below is the rule rather than the exception.
	if ((cb >= 3) && (pb[0] == 0xEF) && (pb[1] == 0xBB) && (pb[2] == 0xBF))
	{
		pb += 3;
		cb -= 3;
	}
	else if ((cb >= 2) && (pb[0] == 0xFF) && (pb[1] == 0xFE))
	{
		out.assign((const wchar_t *) (pb + 2), (cb - 2) / sizeof(wchar_t));
		return out;
	}
	else if ((cb >= 2) && (pb[0] == 0xFE) && (pb[1] == 0xFF))
	{
		// UTF-16BE. Windows has no wide code page for it, so the pairs are
		// swapped by hand rather than converted.
		size_t cch = (cb - 2) / 2;

		out.resize(cch);
		for (size_t i = 0; i < cch; i++)
		{
			out[i] = (wchar_t) (((WORD) pb[2 + i * 2] << 8) | (WORD) pb[3 + i * 2]);
		}
		return out;
	}

	if (cb == 0)
	{
		return out;
	}

	// Strict UTF-8 first. MB_ERR_INVALID_CHARS is what makes the fallback
	// reachable at all: without it an ill-formed sequence is replaced with
	// U+FFFD and the conversion reports success, so a GBK script would turn
	// into replacement characters instead of being read as ANSI.
	{
		int cch = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (LPCCH) pb, (int) cb, NULL, 0);
		UINT uCodePage = CP_UTF8;

		if (cch <= 0)
		{
			uCodePage = CP_ACP;
			cch = MultiByteToWideChar(CP_ACP, 0, (LPCCH) pb, (int) cb, NULL, 0);
		}
		if (cch <= 0)
		{
			return out;
		}

		out.resize((size_t) cch);
		MultiByteToWideChar(uCodePage, 0, (LPCCH) pb, (int) cb, &out[0], cch);
	}
	return out;
}
