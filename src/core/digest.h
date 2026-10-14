#pragma once

#include "base.h"

// MD5. The digest is MD5Str_Len bytes; the hex text is twice that many
// characters, plus a terminator.
#define MD5Str_Len 16

// Scratch buffer that ByteToMD5 formats into and returns.
// A global because every caller formats into the same buffer.
extern TCHAR MD5_Code_Buf[MD5Str_Len * 2 + 1];

// The raw digest, for a caller that wants to format the digits itself: HASH
// spells them upper case and MD5C lower case, and which one is wanted is not the
// digest's business.
void Md5Digest(const BYTE *pbData, UINT uLen, BYTE pbOut[MD5Str_Len]);

PTSTR ByteToMD5(const BYTE pByteBuf[], UINT pBufLen);
PTSTR UStrToMD5(PTSTR ptzCmd);