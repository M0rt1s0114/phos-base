#include "digest.h"

TCHAR MD5_Code_Buf[MD5Str_Len * 2 + 1];

// MD5, written out from the description in RFC 1321.
//
// Windows would compute this for us -- the crypto provider is there, HASH proves
// it -- but the algorithm is short and completely specified, and owning it means
// the password check and the LOGO check do not depend on a provider being
// registered in whatever PE this ends up in.

namespace
{
	// How far the working word rotates in each of the four rounds, and the
	// constants added to it. Both are indexed by the round step.
	const UINT c_rotate[64] =
	{
		7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,
		5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,
		4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,
		6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21
	};

	const DWORD c_add[64] =
	{
		0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
		0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
		0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
		0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
		0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
		0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
		0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
		0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
		0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
		0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
		0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
		0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
		0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
		0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
		0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
		0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
	};

	DWORD RotateLeft(DWORD dwValue, UINT uBits)
	{
		return (dwValue << uBits) | (dwValue >> (32 - uBits));
	}

	// One 64-byte block, folded into the four-word state.
	void MixBlock(DWORD dwState[4], const BYTE pbBlock[64])
	{
		DWORD w[16];
		DWORD a = dwState[0];
		DWORD b = dwState[1];
		DWORD c = dwState[2];
		DWORD d = dwState[3];
		UINT i;

		// The block is read as sixteen little-endian words.
		for (i = 0; i < 16; i++)
		{
			w[i] = (DWORD) pbBlock[i * 4]
				| ((DWORD) pbBlock[i * 4 + 1] << 8)
				| ((DWORD) pbBlock[i * 4 + 2] << 16)
				| ((DWORD) pbBlock[i * 4 + 3] << 24);
		}

		for (i = 0; i < 64; i++)
		{
			DWORD f;
			UINT g;

			if (i < 16)
			{
				f = (b & c) | (~b & d);
				g = i;
			}
			else if (i < 32)
			{
				f = (d & b) | (~d & c);
				g = (5 * i + 1) & 15;
			}
			else if (i < 48)
			{
				f = b ^ c ^ d;
				g = (3 * i + 5) & 15;
			}
			else
			{
				f = c ^ (b | ~d);
				g = (7 * i) & 15;
			}

			f = f + a + c_add[i] + w[g];
			a = d;
			d = c;
			c = b;
			b = b + RotateLeft(f, c_rotate[i]);
		}

		dwState[0] += a;
		dwState[1] += b;
		dwState[2] += c;
		dwState[3] += d;
	}
}

void Md5Digest(const BYTE *pbData, UINT uLen, BYTE pbOut[MD5Str_Len])
{
	// The initial state is the fractional parts of the square roots of 2, 3, 5
	// and 7, in the order the specification gives them.
	DWORD dwState[4] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u };
	BYTE bTail[128];
	ULONGLONG uBits = (ULONGLONG) uLen * 8;
	UINT uWhole = uLen / 64;
	UINT uRest = uLen % 64;
	UINT uTailLen;
	UINT i;

	for (i = 0; i < uWhole; i++)
	{
		MixBlock(dwState, pbData + (i * 64));
	}

	// Whatever did not fill a block, then the 1 bit, then zeros, then the
	// message length as a 64-bit little-endian count of bits. That needs one
	// block when the remainder leaves 8 bytes for the length and two when it
	// does not.
	UMemSet(bTail, 0, sizeof(bTail));
	for (i = 0; i < uRest; i++)
	{
		bTail[i] = pbData[(uWhole * 64) + i];
	}
	bTail[uRest] = 0x80;
	uTailLen = (uRest < 56) ? 64 : 128;
	for (i = 0; i < 8; i++)
	{
		bTail[uTailLen - 8 + i] = (BYTE) (uBits >> (i * 8));
	}
	for (i = 0; i < uTailLen; i += 64)
	{
		MixBlock(dwState, bTail + i);
	}

	// Out again as little-endian words.
	for (i = 0; i < 4; i++)
	{
		pbOut[i * 4]     = (BYTE) (dwState[i]);
		pbOut[i * 4 + 1] = (BYTE) (dwState[i] >> 8);
		pbOut[i * 4 + 2] = (BYTE) (dwState[i] >> 16);
		pbOut[i * 4 + 3] = (BYTE) (dwState[i] >> 24);
	}
}

// Lower case, and the caller wants the digits written over the whole buffer
// rather than appended to whatever was in it before.
PTSTR ByteToMD5(const BYTE pByteBuf[], UINT pBufLen)
{
	static const TCHAR c_hex[] = TEXT("0123456789abcdef");
	BYTE rData[MD5Str_Len];
	UINT i;

	Md5Digest(pByteBuf, pBufLen, rData);
	for (i = 0; i < MD5Str_Len; i++)
	{
		MD5_Code_Buf[i * 2] = c_hex[(rData[i] >> 4) & 0xF];
		MD5_Code_Buf[i * 2 + 1] = c_hex[rData[i] & 0xF];
	}
	MD5_Code_Buf[MD5Str_Len * 2] = 0;
	return MD5_Code_Buf;
}

// The string is taken as the bytes it occupies in the ANSI code page rather than
// as UTF-16 -- that is what the configured password hashes were made against,
// so it is not a detail that can drift.
PTSTR UStrToMD5(PTSTR ptzCmd)
{
	CHAR tzTempBuf[MAX_NAME];
	UINT uLen = 0;

	UStrToAStr(tzTempBuf, ptzCmd, MAX_NAME);
	while (tzTempBuf[uLen])
	{
		uLen++;
	}
	return ByteToMD5((const BYTE *) tzTempBuf, uLen);
}