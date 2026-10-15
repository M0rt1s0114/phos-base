//
// Path and string commands.
//
//   FDIR  directory (no trailing backslash)   FEXT  extension (without the dot)
//   NAME  base name (-ext keeps it)           FNAM  alias of NAME
//   LSTR  N characters from the left          RSTR  N characters from the right
//   STRL  length, in Unicode characters       LPOS  substring or character position
//   MSTR  substring by position or by field   SED   replace
//
// They all take "<variable>=..." and none of them touch the machine.
//
// The exact shapes come from the documented form: FDIR answers without a
// trailing backslash, FEXT without the dot, NAME drops the extension unless
// -ext is given, and STRL counts Unicode characters (not bytes) unless -m/-8
// asks for the multibyte length.
//

// Store a command's result. The documented spelling is "&var", a PE variable; a
// bare name goes to the environment, the same split SET uses.
static BOOL StoreResult(PCTSTR ptzName, PCTSTR ptzValue)
{
	if (ptzName[0] == '&')
	{
		return PeVar::Set(ptzName, ptzValue);
	}
	{
		TCHAR tzPair[MAX_STR];
		UStrPrint(tzPair, TEXT("%s=%s"), ptzName, ptzValue);
		SetEnvironment(tzPair, FALSE, FALSE);
	}
	return TRUE;
}

// "<name>=<rest>" -> the name, and the rest in place. Returns the rest.
static PTSTR SplitNameValue(PTSTR ptzCmd, PTSTR *pptzName)
{
	PTSTR ptzValue = UStrChr(ptzCmd, '=');

	if (!ptzValue)
	{
		*pptzName = NULL;
		return NULL;
	}
	*ptzValue++ = 0;
	*pptzName = UStrTrim(ptzCmd);
	return UStrTrim(ptzValue);
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// FDIR / FEXT / FNAM / NAME / SIZE-style path splits
//////////////////////////////////////////////////////////////////////////////////////////////////

// The directory part, without the trailing backslash ("C:\Windows\System32").
HRESULT Fdir(CoreContext& ctx, PTSTR ptzCmd)
{
	PTSTR ptzName = NULL;
	PTSTR ptzValue = SplitNameValue(ptzCmd, &ptzName);
	TCHAR tzPath[MAX_PATH];

	if (!ptzName || !ptzValue)
	{
		return E_INVALIDARG;
	}
	if (!UStrCopyTo(tzPath, ptzValue))
	{
		return E_INVALIDARG;
	}
	UDirSplitPath(tzPath);            // trims the name off in place
	return StoreResult(ptzName, tzPath) ? S_OK : S_FALSE;
}

// The extension without the dot ("exe"); empty when there is none.
HRESULT Fext(CoreContext& ctx, PTSTR ptzCmd)
{
	PTSTR ptzName = NULL;
	PTSTR ptzValue = SplitNameValue(ptzCmd, &ptzName);
	TCHAR tzPath[MAX_PATH];
	PTSTR ptzDot;

	if (!ptzName || !ptzValue)
	{
		return E_INVALIDARG;
	}
	// Only the name part counts: a dot in a directory name is not an extension.
	if (!UStrCopyTo(tzPath, ptzValue))
	{
		return E_INVALIDARG;
	}
	ptzDot = UDirSplitPath(tzPath);       // returns the name part
	ptzDot = UStrRChr(ptzDot, '.');
	return StoreResult(ptzName, ptzDot ? ptzDot + 1 : TEXT("")) ? S_OK : S_FALSE;
}

// The base name; -ext keeps the extension. NAME is the same command.
static HRESULT NameOrFnam(CoreContext& ctx, PTSTR ptzCmd)
{
	BOOL bKeepExt = FALSE;
	PTSTR ptzName = NULL;
	PTSTR ptzValue;
	TCHAR tzPath[MAX_PATH];
	PTSTR ptzBase;
	PTSTR ptzDot;

	if (!UStrCmpNI(ptzCmd, TEXT("-ext"), 4))
	{
		bKeepExt = TRUE;
		ptzCmd = UStrTrim(ptzCmd + 4);
		if (*ptzCmd == ' ')
		{
			ptzCmd = UStrTrim(ptzCmd + 1);
		}
	}
	ptzValue = SplitNameValue(ptzCmd, &ptzName);
	if (!ptzName || !ptzValue)
	{
		return E_INVALIDARG;
	}
	if (!UStrCopyTo(tzPath, ptzValue))
	{
		return E_INVALIDARG;
	}
	ptzBase = UDirSplitPath(tzPath);
	if (!bKeepExt)
	{
		ptzDot = UStrRChr(ptzBase, '.');
		if (ptzDot)
		{
			*ptzDot = 0;
		}
	}
	return StoreResult(ptzName, ptzBase) ? S_OK : S_FALSE;
}

HRESULT Fnam(CoreContext& ctx, PTSTR ptzCmd)
{
	return NameOrFnam(ctx, ptzCmd);
}

HRESULT Name(CoreContext& ctx, PTSTR ptzCmd)
{
	return NameOrFnam(ctx, ptzCmd);
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// LSTR / RSTR / STRL
//////////////////////////////////////////////////////////////////////////////////////////////////

// "count,string" -> the count and the string.
static BOOL SplitCountText(PTSTR ptzValue, INT *piCount, PTSTR *pptzText)
{
	PTSTR ptzComma = UStrChr(ptzValue, ',');

	if (!ptzComma)
	{
		return FALSE;
	}
	*ptzComma++ = 0;
	*piCount = UStrToInt(UStrTrim(ptzValue));
	*pptzText = UStrTrim(ptzComma);
	return TRUE;
}

// Leftmost N characters. N < 1 or past the end gives the whole string back.
HRESULT Lstr(CoreContext& ctx, PTSTR ptzCmd)
{
	BOOL bFromVar = FALSE;
	PTSTR ptzName = NULL;
	PTSTR ptzValue;
	INT iCount = 0;
	TCHAR tzText[MAX_STR];

	if (*ptzCmd == '*')
	{
		bFromVar = TRUE;
		ptzCmd = UStrTrim(ptzCmd + 1);
	}
	ptzValue = SplitNameValue(ptzCmd, &ptzName);
	if (!ptzName || !ptzValue || !SplitCountText(ptzValue, &iCount, &ptzValue))
	{
		return E_INVALIDARG;
	}
	if (bFromVar)
	{
		// "*" = the count came from a variable. One that is not there used to
		// leave this buffer uninitialised and the count was read out of whatever
		// was on the stack; answer -1, as the numeric form does.
		if (!PeVar::Get(ptzValue, tzText, _NumOf(tzText)))
		{
			return StoreResult(ptzName, TEXT("-1")) ? S_OK : S_FALSE;
		}
		ptzValue = tzText;
	}
	if ((iCount < 1) || ((UINT) iCount > UStrLen(ptzValue)))
	{
		iCount = (INT) UStrLen(ptzValue);
	}
	UStrCopyN(tzText, ptzValue, (UINT) iCount + 1);
	return StoreResult(ptzName, tzText) ? S_OK : S_FALSE;
}

// Rightmost N characters.
HRESULT Rstr(CoreContext& ctx, PTSTR ptzCmd)
{
	PTSTR ptzName = NULL;
	PTSTR ptzValue;
	INT iCount = 0;
	UINT uLen;

	ptzValue = SplitNameValue(ptzCmd, &ptzName);
	if (!ptzName || !ptzValue || !SplitCountText(ptzValue, &iCount, &ptzValue))
	{
		return E_INVALIDARG;
	}
	uLen = UStrLen(ptzValue);
	if ((iCount < 1) || ((UINT) iCount > uLen))
	{
		iCount = (INT) uLen;
	}
	return StoreResult(ptzName, ptzValue + (uLen - (UINT) iCount)) ? S_OK : S_FALSE;
}

// Length. Unicode characters by default, bytes with -m (multibyte) or -8.
HRESULT Strl(CoreContext& ctx, PTSTR ptzCmd)
{
	BOOL bFromVar = FALSE;
	BOOL bBytes = FALSE;
	PTSTR ptzName = NULL;
	PTSTR ptzValue;
	TCHAR tzText[MAX_STR];
	TCHAR tzNumber[32];

	while (ptzCmd[0] == '-')
	{
		PTSTR ptzNext = UStrChr(ptzCmd, ' ');
		if (!UStrCmpNI(ptzCmd, TEXT("-m"), 2) || !UStrCmpNI(ptzCmd, TEXT("-8"), 2))
		{
			bBytes = TRUE;
		}
		if (!ptzNext)
		{
			return E_INVALIDARG;
		}
		ptzCmd = UStrTrim(ptzNext + 1);
	}
	if (*ptzCmd == '*')
	{
		bFromVar = TRUE;
		ptzCmd = UStrTrim(ptzCmd + 1);
	}
	ptzValue = SplitNameValue(ptzCmd, &ptzName);
	if (!ptzName || !ptzValue)
	{
		return E_INVALIDARG;
	}
	if (bFromVar)
	{
		if (!PeVar::Get(ptzValue, tzText, _NumOf(tzText)))
		{
			// The documentation: a variable that is not there answers -1.
			return StoreResult(ptzName, TEXT("-1")) ? S_OK : S_FALSE;
		}
		ptzValue = tzText;
	}
	if (bBytes)
	{
		CHAR tzAnsi[MAX_STR * 2];
		UINT uBytes = WideCharToMultiByte(CP_ACP, 0, ptzValue, -1, tzAnsi, _NumOf(tzAnsi), NULL, NULL);
		UStrPrint(tzNumber, TEXT("%u"), uBytes ? uBytes - 1 : 0);
	}
	else
	{
		UStrPrint(tzNumber, TEXT("%u"), UStrLen(ptzValue));
	}
	return StoreResult(ptzName, tzNumber) ? S_OK : S_FALSE;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// LPOS
//////////////////////////////////////////////////////////////////////////////////////////////////

// LPOS [*] [-case] <variable>=<character>,<number>,<string>
//
// The position of the <number>-th occurrence of <character>, counting from the left. With
// the leading "*" the thing being looked for is a substring rather than a single
// character. 0 means "not found", and the docs match either case by default.
HRESULT Lpos(CoreContext& ctx, PTSTR ptzCmd)
{
	BOOL bSubstring = FALSE;
	BOOL bCase = FALSE;
	PTSTR ptzName = NULL;
	PTSTR ptzValue;
	PTSTR ptzNeedle;
	PTSTR ptzRest;
	INT iCount = 1;
	PTSTR ptzFound;
	UINT uPos = 0;
	TCHAR tzNumber[32];

	if (*ptzCmd == '*')
	{
		bSubstring = TRUE;
		ptzCmd = UStrTrim(ptzCmd + 1);
	}
	while (!UStrCmpNI(ptzCmd, TEXT("-case"), 5))
	{
		bCase = TRUE;
		ptzCmd = UStrTrim(ptzCmd + 5);
	}
	ptzValue = SplitNameValue(ptzCmd, &ptzName);
	if (!ptzName || !ptzValue)
	{
		return E_INVALIDARG;
	}
	ptzNeedle = ptzValue;
	ptzRest = UStrChr(ptzNeedle, ',');
	if (!ptzRest)
	{
		return E_INVALIDARG;
	}
	*ptzRest++ = 0;
	{
		PTSTR ptzComma = UStrChr(ptzRest, ',');
		if (!ptzComma)
		{
			return E_INVALIDARG;
		}
		*ptzComma++ = 0;
		iCount = UStrToInt(UStrTrim(ptzRest));
		if (iCount < 1)
		{
			iCount = 1;
		}
		ptzRest = ptzComma;
	}

	if (!bSubstring)
	{
		// A single character, compared one code unit at a time.
		TCHAR cNeedle = ptzNeedle[0];
		UINT uFound = 0;
		for (UINT i = 0; ptzRest[i]; i++)
		{
			TCHAR a = ptzRest[i];
			TCHAR b = cNeedle;
			BOOL bMatch;
			if (!bCase)
			{
				a = (TCHAR) ((a >= 'a') && (a <= 'z') ? a - 32 : a);
				b = (TCHAR) ((b >= 'a') && (b <= 'z') ? b - 32 : b);
			}
			bMatch = (a == b);
			if (bMatch)
			{
				uFound++;
				if ((INT) uFound == iCount)
				{
					uPos = i + 1;
					break;
				}
			}
		}
	}
	else
	{
		UINT uNeedleLen = UStrLen(ptzNeedle);
		UINT uFound = 0;
		if (uNeedleLen)
		{
			for (PTSTR p = ptzRest; *p; p++)
			{
				BOOL bMatch = bCase
					? (UStrCmpN(p, ptzNeedle, uNeedleLen) == 0)
					: (UStrCmpNI(p, ptzNeedle, uNeedleLen) == 0);
				if (bMatch)
				{
					uFound++;
					if ((INT) uFound == iCount)
					{
						uPos = (UINT) (p - ptzRest) + 1;
						break;
					}
				}
			}
		}
	}

	ptzFound = tzNumber;
	UStrPrint(tzNumber, TEXT("%u"), uPos);
	return StoreResult(ptzName, ptzFound) ? S_OK : S_FALSE;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// MSTR
//////////////////////////////////////////////////////////////////////////////////////////////////

// The field table of a string: one entry per field, in the original string.
struct MstrFields
{
	UINT  uCount;
	UINT  uStart[MAX_STR];
	UINT  uEnd[MAX_STR];
};

// Default separator is a space, "*" is a TAB and "$" is a run of spaces.
static VOID MstrSplit(PCTSTR ptzText, INT iMode, MstrFields *pFields)
{
	UINT i = 0;
	BOOL bInField = FALSE;
	UINT uStart = 0;

	pFields->uCount = 0;
	for (i = 0; ; i++)
	{
		BOOL bSeparator;
		if (ptzText[i] == 0)
		{
			bSeparator = TRUE;
		}
		else if (iMode == 1)
		{
			bSeparator = (ptzText[i] == '\t');
		}
		else
		{
			bSeparator = (ptzText[i] == ' ');
		}
		if (!bSeparator)
		{
			if (!bInField)
			{
				uStart = i;
				bInField = TRUE;
			}
			continue;
		}
		if (bInField)
		{
			if (pFields->uCount < _NumOf(pFields->uStart))
			{
				pFields->uStart[pFields->uCount] = uStart;
				pFields->uEnd[pFields->uCount] = i;
				pFields->uCount++;
			}
			bInField = FALSE;
		}
		if (ptzText[i] == 0)
		{
			break;
		}
	}
}

// MSTR [*|$] [-delims:C] <variable>=<position>,<length>,<string>   (form 1)
// MSTR [*|$] [-delims:C] <variable>=<field numbers...><string>      (forms 2 and 3)
//
// Form 1 is "from <position>, <length> characters". Forms 2/3 pull fields out by number:
// <n> is one field, <n-m> is a run of them joined back with the separator, <n*>
// is one field and <#> is the number of fields. What is implemented here is what
// a script reaches for first.
HRESULT Mstr(CoreContext& ctx, PTSTR ptzCmd)
{
	INT iMode = 0;                 // 0 = space, 1 = TAB, 2 = space runs
	PTSTR ptzNames = NULL;
	PTSTR ptzValue;
	TCHAR tzText[MAX_STR];

	if (*ptzCmd == '*')
	{
		iMode = 1;
		ptzCmd = UStrTrim(ptzCmd + 1);
	}
	else if (*ptzCmd == '$')
	{
		iMode = 2;
		ptzCmd = UStrTrim(ptzCmd + 1);
	}
	if (!UStrCmpNI(ptzCmd, TEXT("-delims:"), 8))
	{
		// Custom separators are not implemented; fail rather than split wrongly.
		return E_INVALIDARG;
	}
	ptzValue = SplitNameValue(ptzCmd, &ptzNames);
	if (!ptzNames || !ptzValue)
	{
		return E_INVALIDARG;
	}

	if (ptzValue[0] != '<')
	{
		// Form 1: position,length,string
		INT iPos = 0;
		INT iLen = 0;
		PTSTR ptzComma = UStrChr(ptzValue, ',');
		PTSTR ptzStart;
		if (!ptzComma)
		{
			return E_INVALIDARG;
		}
		*ptzComma++ = 0;
		iPos = UStrToInt(UStrTrim(ptzValue));
		ptzComma = UStrTrim(ptzComma);
		ptzStart = UStrChr(ptzComma, ',');
		if (!ptzStart)
		{
			return E_INVALIDARG;
		}
		*ptzStart++ = 0;
		iLen = UStrToInt(UStrTrim(ptzComma));
		ptzStart = UStrTrim(ptzStart);
		if (iPos < 0)
		{
			iPos = 0;
		}
		if ((UINT) iPos > UStrLen(ptzStart))
		{
			iPos = (INT) UStrLen(ptzStart);
		}
		if ((iLen < 0) || ((UINT) (iPos + iLen) > UStrLen(ptzStart)))
		{
			iLen = (INT) UStrLen(ptzStart) - iPos;
		}
		UStrCopyN(tzText, ptzStart + iPos, (UINT) iLen + 1);
		return StoreResult(ptzNames, tzText) ? S_OK : S_FALSE;
	}

	// Forms 2/3: "<...>string"
	{
		TCHAR tzSelector[MAX_NAME];
		UINT uSel = 0;
		MstrFields fields;
		PCTSTR ptzSource;

		ptzValue++;
		while ((ptzValue[uSel]) && (ptzValue[uSel] != '>') && (uSel + 1 < _NumOf(tzSelector)))
		{
			tzSelector[uSel] = ptzValue[uSel];
			uSel++;
		}
		tzSelector[uSel] = 0;
		if (ptzValue[uSel] != '>')
		{
			return E_INVALIDARG;
		}
		ptzSource = ptzValue + uSel + 1;
		MstrSplit(ptzSource, iMode, &fields);

		if (!UStrCmpI(tzSelector, TEXT("#")))
		{
			TCHAR tzNumber[32];
			UStrPrint(tzNumber, TEXT("%u"), fields.uCount);
			return StoreResult(ptzNames, tzNumber) ? S_OK : S_FALSE;
		}

			// <n>, <n*>, <n->; a negative number counts from the end. The "-"
			// means "from here to the end of the string", not a range: the
			// text form answers the whole string for "<1-2>" as well.
		{
			INT iFrom = UStrToInt(tzSelector);
			INT iTo = iFrom;
			BOOL bSingle = FALSE;
				BOOL bToEnd = FALSE;
			PTSTR ptzTail = tzSelector;
			UINT uStart;
			UINT uEnd;
			UINT i;
			UINT uOut = 0;

			// Stop at the first thing that is not a digit: it is the '*' or the
			// '-' that says which shape the selector is.
			while (UChrIsNum(*ptzTail))
			{
				ptzTail++;
			}
			if (*ptzTail == '*')
			{
				bSingle = TRUE;
			}
			else if (*ptzTail == '-')
			{
					bToEnd = TRUE;
			}
			if (iFrom < 0)
			{
				iFrom = (INT) fields.uCount + iFrom + 1;
			}
			if (iTo < 0)
			{
				iTo = (INT) fields.uCount + iTo + 1;
			}
			if (iFrom < 1)
			{
				iFrom = 1;
			}
			if (iTo > (INT) fields.uCount)
			{
				iTo = (INT) fields.uCount;
			}
				if (bToEnd)
				{
					iTo = (INT) fields.uCount;
				}
			if (iFrom > iTo)
			{
				return StoreResult(ptzNames, TEXT("")) ? S_OK : S_FALSE;
			}
			uStart = fields.uStart[iFrom - 1];
			uEnd = fields.uEnd[iTo - 1];
			if (bSingle)
			{
				uEnd = fields.uEnd[iFrom - 1];
			}
			for (i = uStart; (i < uEnd) && (uOut + 1 < _NumOf(tzText)); i++)
			{
				tzText[uOut++] = ptzSource[i];
			}
			tzText[uOut] = 0;
		}
		return StoreResult(ptzNames, tzText) ? S_OK : S_FALSE;
	}
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// SED
//////////////////////////////////////////////////////////////////////////////////////////////////

// Does the pattern need a regex engine? A literal replace is exact and useful;
// a pattern that means something as a regex (a "." matches any character, ".*"
// is greedy, brackets group) would silently do the wrong thing here, so those
// are refused instead.
static BOOL SedLooksRegex(PCTSTR ptzPattern)
{
	for (PCTSTR p = ptzPattern; *p; p++)
	{
		if ((*p == '.') || (*p == '*') || (*p == '+') || (*p == '?') || (*p == '[') ||
			(*p == ']') || (*p == '(') || (*p == ')') || (*p == '|') || (*p == '^') ||
			(*p == '$') || (*p == '\\'))
		{
			return TRUE;
		}
	}
	return FALSE;
}

// SED [-ni] [-t|-ts] <variable>=<count>,<find>,<replace>,<source>
//
// Literal replacement only, for the reason above; a count of 0 means every match.
HRESULT Sed(CoreContext& ctx, PTSTR ptzCmd)
{
	BOOL bIgnoreCase = FALSE;
	BOOL bLiteral = FALSE;
	PTSTR ptzName = NULL;
	PTSTR ptzValue;
	INT iCount = 0;
	PTSTR ptzPattern;
	PTSTR ptzReplace;
	PTSTR ptzSource;
	TCHAR tzOut[MAX_STR];
	UINT uOut = 0;
	UINT uPattern;

	while (ptzCmd[0] == '-')
	{
		PTSTR ptzNext;
		if (!UStrCmpNI(ptzCmd, TEXT("-ni"), 3))
		{
			bIgnoreCase = TRUE;
		}
		else if (!UStrCmpNI(ptzCmd, TEXT("-t"), 2))
		{
			return E_INVALIDARG;      // translation modes come later
		}
		ptzNext = UStrChr(ptzCmd, ' ');
		ptzCmd = ptzNext ? UStrTrim(ptzNext + 1) : ptzCmd + UStrLen(ptzCmd);
	}
	ptzValue = SplitNameValue(ptzCmd, &ptzName);
	if (!ptzName || !ptzValue)
	{
		return E_INVALIDARG;
	}
	// count,find,replace,source
	ptzPattern = UStrChr(ptzValue, ',');
	if (!ptzPattern)
	{
		return E_INVALIDARG;
	}
	*ptzPattern++ = 0;
	if (*ptzValue == '*')
	{
		bLiteral = TRUE;              // the "*" flag lives in the count field
		ptzValue++;
	}
	iCount = UStrToInt(UStrTrim(ptzValue));

	ptzReplace = UStrChr(ptzPattern, ',');
	if (!ptzReplace)
	{
		return E_INVALIDARG;
	}
	*ptzReplace++ = 0;

	ptzSource = UStrChr(ptzReplace, ',');
	if (!ptzSource)
	{
		return E_INVALIDARG;
	}
	*ptzSource++ = 0;

	// A pattern that only means something as a regex is refused: the text form
	// would match it as one, and doing a literal replace would be a silent
	// difference rather than a failure.
	if ((!bLiteral && SedLooksRegex(ptzPattern)) || (ptzPattern[0] == 0))
	{
		return E_INVALIDARG;
	}

	uPattern = UStrLen(ptzPattern);
	{
		UINT uLength = UStrLen(ptzSource);
		UINT i;
		INT iDone = 0;

		for (i = 0; (i < uLength) && (uOut + 1 < _NumOf(tzOut)); )
		{
			BOOL bMatch = FALSE;

			if (i + uPattern <= uLength)
			{
				bMatch = bIgnoreCase
					? (UStrCmpNI(ptzSource + i, ptzPattern, uPattern) == 0)
					: (UStrCmpN(ptzSource + i, ptzPattern, uPattern) == 0);
			}
			if (bMatch && ((iCount == 0) || (iDone < iCount)))
			{
				UINT k;
				for (k = 0; (ptzReplace[k]) && (uOut + 1 < _NumOf(tzOut)); k++)
				{
					tzOut[uOut++] = ptzReplace[k];
				}
				i += uPattern;
				iDone++;
				continue;
			}
			tzOut[uOut++] = ptzSource[i++];
		}
	}
	tzOut[uOut] = 0;
	return StoreResult(ptzName, tzOut) ? S_OK : S_FALSE;
}
