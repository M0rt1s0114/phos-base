//
// BASE and HASH.
//
//   BASE* [-u] <string>,<&var>          standard base64, -u decodes
//   HASH <file>|< $string >|<*PEvar>,<&var>[,CRC32|MD5|SHA1|SHA256]
//
// BASE without the "*" is a variant whose mapping has not been established, so
// the plain form refuses rather than answering with standard base64 and calling
// that the same thing.
//
// HASH takes a file, a literal string after "$", or a PE variable after "*".
// The digests are lowercase hex, which is what the documented form shows
// for the empty string (d41d8cd98f00b204e9800998ecf8427e).
//

#include "../core/digest.h"

static const TCHAR c_b64Alphabet[] =
	TEXT("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/");

static INT B64Index(TCHAR c)
{
	for (INT i = 0; i < 64; i++)
	{
		if (c_b64Alphabet[i] == c)
		{
			return i;
		}
	}
	if (c == '=')
	{
		return -2;
	}
	return -1;
}

static VOID Base64Encode(const BYTE *pbData, UINT uSize, PTSTR ptzOut, UINT uOut)
{
	UINT uIn = 0;
	UINT uAt = 0;

	while ((uIn + 2 < uSize) && (uAt + 5 < uOut))
	{
		UINT uTriple = (pbData[uIn] << 16) | (pbData[uIn + 1] << 8) | pbData[uIn + 2];
		ptzOut[uAt++] = c_b64Alphabet[(uTriple >> 18) & 0x3F];
		ptzOut[uAt++] = c_b64Alphabet[(uTriple >> 12) & 0x3F];
		ptzOut[uAt++] = c_b64Alphabet[(uTriple >> 6) & 0x3F];
		ptzOut[uAt++] = c_b64Alphabet[uTriple & 0x3F];
		uIn += 3;
	}
	if ((uIn < uSize) && (uAt + 5 < uOut))
	{
		UINT uTriple = pbData[uIn] << 16;
		BOOL bTwo = (uIn + 1 < uSize);
		if (bTwo)
		{
			uTriple |= pbData[uIn + 1] << 8;
		}
		ptzOut[uAt++] = c_b64Alphabet[(uTriple >> 18) & 0x3F];
		ptzOut[uAt++] = c_b64Alphabet[(uTriple >> 12) & 0x3F];
		ptzOut[uAt++] = bTwo ? c_b64Alphabet[(uTriple >> 6) & 0x3F] : '=';
		ptzOut[uAt++] = '=';
	}
	ptzOut[uAt] = 0;
}

static BOOL Base64Decode(PCTSTR ptzText, PSTR ptzOut, UINT uOut)
{
	UINT uAt = 0;
	UINT uIn = 0;

	while (ptzText[uIn] && (uAt + 1 < uOut))
	{
		INT a = B64Index(ptzText[uIn]);
		INT b;
		INT c;
		INT d;
		UINT uTriple;
		UINT uBytes;

		if (a < 0)
		{
			return FALSE;
		}
		uIn++;
		b = B64Index(ptzText[uIn]);
		if (b < 0)
		{
			return FALSE;
		}
		uIn++;
		if (ptzText[uIn])
		{
			c = B64Index(ptzText[uIn]);
			uIn++;
		}
		else
		{
			c = -2;
		}
		if (ptzText[uIn])
		{
			d = B64Index(ptzText[uIn]);
			uIn++;
		}
		else
		{
			d = -2;
		}
		if (c == -2)
		{
			uTriple = (UINT) a << 18 | (UINT) b << 12;
			uBytes = 1;
		}
		else if (d == -2)
		{
			uTriple = (UINT) a << 18 | (UINT) b << 12 | (UINT) c << 6;
			uBytes = 2;
		}
		else
		{
			uTriple = (UINT) a << 18 | (UINT) b << 12 | (UINT) c << 6 | (UINT) d;
			uBytes = 3;
		}
		if (uAt + uBytes >= uOut)
		{
			return FALSE;
		}
		ptzOut[uAt++] = (CHAR) ((uTriple >> 16) & 0xFF);
		if (uBytes >= 2)
		{
			ptzOut[uAt++] = (CHAR) ((uTriple >> 8) & 0xFF);
		}
		if (uBytes >= 3)
		{
			ptzOut[uAt++] = (CHAR) (uTriple & 0xFF);
		}
	}
	ptzOut[uAt] = 0;
	return TRUE;
}

HRESULT Base(CoreContext& ctx, PTSTR ptzCmd)
{
	BOOL bStandard = FALSE;
	BOOL bDecode = FALSE;
	PTSTR ptzText;
	PTSTR ptzVar;
	TCHAR tzOut[MAX_STR];

	if (*ptzCmd == '*')
	{
		bStandard = TRUE;
		ptzCmd++;
	}
	else
	{
		// The text-command dialect's plain form uses a mapping that is not
		// established, so it refuses rather than answering with standard base64
		// and calling that the same thing.
		return E_INVALIDARG;
	}
	if (*ptzCmd == '-')
	{
		if (!UStrCmpNI(ptzCmd, TEXT("-u"), 2))
		{
			bDecode = TRUE;
			ptzCmd += 2;
		}
		else
		{
			return E_INVALIDARG;
		}
	}
	ptzCmd = UStrTrim(ptzCmd);
	ptzVar = UStrRChr(ptzCmd, ',');
	if (!ptzVar)
	{
		return E_INVALIDARG;
	}
	*ptzVar++ = 0;
	ptzText = UStrTrim(ptzCmd);
	ptzVar = UStrTrim(ptzVar);

	if (bDecode)
	{
		CHAR tzBytes[MAX_STR];
		TCHAR tzWide[MAX_STR];
		if (!Base64Decode(ptzText, tzBytes, _NumOf(tzBytes)))
		{
			return E_INVALIDARG;
		}
		// The decoded bytes are text in the ANSI code page, the same convention
		// the encoding side uses.
		MultiByteToWideChar(CP_ACP, 0, tzBytes, -1, tzWide, _NumOf(tzWide));
		UStrCopy(tzOut, tzWide);
	}
	else
	{
		// The bytes of the string, not its UTF-16 form: standard base64 of
		// "hello" is "aGVsbG8=", and that is what the case checks.
		CHAR tzBytes[MAX_STR];
		INT n = WideCharToMultiByte(CP_ACP, 0, ptzText, -1, tzBytes, _NumOf(tzBytes), NULL, NULL);
		Base64Encode((const BYTE *) tzBytes, (n > 0) ? (UINT) (n - 1) : 0, tzOut, _NumOf(tzOut));
	}
	return StoreResult(ptzVar, tzOut) ? S_OK : S_FALSE;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// HASH
//////////////////////////////////////////////////////////////////////////////////////////////////

// CRC32, the usual reflected polynomial. The text form reports the empty string
// as "00000000", which is this one with the initial and final complement.
static DWORD Crc32(const BYTE *pbData, UINT uSize)
{
	DWORD dwCrc = 0xFFFFFFFF;
	UINT i;
	UINT k;

	for (i = 0; i < uSize; i++)
	{
		dwCrc ^= pbData[i];
		for (k = 0; k < 8; k++)
		{
			dwCrc = (dwCrc >> 1) ^ (0xEDB88320u & (0u - (dwCrc & 1u)));
		}
	}
	return dwCrc ^ 0xFFFFFFFF;
}

static VOID BytesToHex(const BYTE *pbData, UINT uSize, PTSTR ptzOut, UINT uOut)
{
	UINT i;
	UINT uAt = 0;
	// Upper case: the text form answers 5D41402ABC4B2A76B9719D911017C592 for
	// MD5 of "hello", whatever the help's prose shows for the empty string.
	static const TCHAR c_hex[] = TEXT("0123456789ABCDEF");
	for (i = 0; (i < uSize) && (uAt + 3 < uOut); i++)
	{
		UINT uByte = pbData[i];
		ptzOut[uAt++] = c_hex[(uByte >> 4) & 0xF];
		ptzOut[uAt++] = c_hex[uByte & 0xF];
	}
	ptzOut[uAt] = 0;
}

// MD5, SHA1 and SHA256 through the crypto provider, with the base provider.
// That choice is observable: asking the base provider for SHA256 answers
// NTE_BAD_ALGID (0x80090008), and the text form reports exactly that in this PE,
// so the AES provider is deliberately NOT tried.
static HRESULT CryptoDigest(PCTSTR ptzType, const BYTE *pbData, UINT uSize, PTSTR ptzOut, UINT uOut)
{
	ALG_ID id;
	HCRYPTPROV hProv = 0;
	HCRYPTHASH hHash = 0;
	BYTE bDigest[64];
	DWORD dwLen = _NumOf(bDigest);
	HRESULT hResult;

	if (!UStrCmpI(ptzType, TEXT("MD5")))
	{
		// MD5 comes from core/digest.cpp rather than from the provider, so one
		// implementation serves HASH and MD5C both. The two differ only in the
		// case of the hex digits, which BytesToHex decides.
		Md5Digest(pbData, uSize, bDigest);
		BytesToHex(bDigest, MD5Str_Len, ptzOut, uOut);
		return S_OK;
	}
	else if (!UStrCmpI(ptzType, TEXT("SHA1")))
	{
		id = CALG_SHA1;
	}
	else if (!UStrCmpI(ptzType, TEXT("SHA256")))
	{
		// The header only declares this from Vista on, and the project is
		// pinned below that; the value is the documented one.
#ifndef CALG_SHA_256
#define CALG_SHA_256 0x0000800c
#endif
		id = CALG_SHA_256;
	}
	else
	{
		return E_INVALIDARG;
	}
	if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
	{
		return (HRESULT) GetLastError();
	}
	if (!CryptCreateHash(hProv, id, 0, 0, &hHash))
	{
		hResult = (HRESULT) GetLastError();
		CryptReleaseContext(hProv, 0);
		return hResult;
	}
	hResult = E_FAIL;
	if (CryptHashData(hHash, pbData, uSize, 0))
	{
		if (CryptGetHashParam(hHash, HP_HASHVAL, bDigest, &dwLen, 0))
		{
			BytesToHex(bDigest, dwLen, ptzOut, uOut);
			hResult = S_OK;
		}
		else
		{
			hResult = (HRESULT) GetLastError();
		}
	}
	else
	{
		hResult = (HRESULT) GetLastError();
	}
	CryptDestroyHash(hHash);
	CryptReleaseContext(hProv, 0);
	return hResult;
}

HRESULT Hash(CoreContext& ctx, PTSTR ptzCmd)
{
	BOOL bFromVar = FALSE;
	BOOL bFromString = FALSE;
	PTSTR ptzSource;
	PTSTR ptzVar;
	PTSTR ptzType = NULL;
	TCHAR tzType[MAX_NAME];
	TCHAR tzHex[MAX_STR];
	PBYTE pbData = NULL;
	UINT uSize = 0;
	PBYTE pbFree = NULL;

	if (*ptzCmd == '*')
	{
		bFromVar = TRUE;
		ptzCmd++;
	}
	else if (*ptzCmd == '$')
	{
		bFromString = TRUE;
		ptzCmd++;
	}
	ptzSource = ptzCmd;
	ptzVar = UStrChr(ptzSource, ',');
	if (!ptzVar)
	{
		return E_INVALIDARG;
	}
	*ptzVar++ = 0;
	ptzType = UStrChr(ptzVar, ',');
	if (ptzType)
	{
		*ptzType++ = 0;
		if (!UStrCopyTo(tzType, UStrTrim(ptzType)))
		{
			return E_INVALIDARG;
		}
	}
	else
	{
		UStrCopyTo(tzType, TEXT("MD5"));
	}
	ptzSource = UStrTrim(ptzSource);
	ptzVar = UStrTrim(ptzVar);

	if (bFromVar)
	{
		TCHAR tzValue[MAX_STR];
		if (!PeVar::Get(ptzSource, tzValue, _NumOf(tzValue)))
		{
			return StoreResult(ptzVar, TEXT("")) ? S_OK : S_FALSE;
		}
		pbData = (PBYTE) tzValue;
		uSize = UStrLen(tzValue) * sizeof(TCHAR);
	}
	else if (bFromString)
	{
		// ASCII mode: the bytes of the string in the ANSI code page.
		UINT uBytes = WideCharToMultiByte(CP_ACP, 0, ptzSource, -1, NULL, 0, NULL, NULL);
		uBytes = uBytes ? uBytes - 1 : 0;
		pbFree = (PBYTE) UMemAlloc(uBytes + 1);
		if (!pbFree)
		{
			return S_FALSE;
		}
		WideCharToMultiByte(CP_ACP, 0, ptzSource, -1, (PSTR) pbFree, uBytes + 1, NULL, NULL);
		pbData = pbFree;
		uSize = uBytes;
	}
	else
	{
		UINT uRead = (UINT) -1;
		PBYTE pbFile = (PBYTE) UFileLoad(ptzSource, NULL, &uRead);
		if (!pbFile)
		{
			UMemFree(pbFree);
			return StoreResult(ptzVar, TEXT("")) ? S_OK : S_FALSE;
		}
		pbData = pbFile;
		uSize = uRead;
		pbFree = pbFile;       // freed below either way
	}

	if (!UStrCmpI(tzType, TEXT("CRC32")))
	{
		DWORD dwCrc = Crc32(pbData, uSize);
		_snwprintf_s(tzHex, _NumOf(tzHex), _TRUNCATE, TEXT("%08X"), dwCrc);
	}
	else
	{
		HRESULT hResult = CryptoDigest(tzType, pbData, uSize, tzHex, _NumOf(tzHex));
		if (hResult != S_OK)
		{
			UMemFree(pbFree);
			return hResult;
		}
	}
	UMemFree(pbFree);
	return StoreResult(ptzVar, tzHex) ? S_OK : S_FALSE;
}
