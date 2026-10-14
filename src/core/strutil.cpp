#include "strutil.h"

// Whitespace here means the space and the tab. That is the whole definition the
// callers were written against.

PTSTR UStrLeftTrim(PTSTR ptstr)
{
	if (!ptstr)
	{
		return NULL;
	}
	while ((*ptstr == ' ') || (*ptstr == '\t'))
	{
		ptstr++;
	}
	return ptstr;
}

// Trims from the right by writing terminators over the characters it drops, so
// the caller's buffer really does get shorter. It returns the pointer it was
// handed rather than the trimmed start, because callers pass in the result of
// UStrLeftTrim and expect that same pointer back.
PTSTR UStrRightTrim(PTSTR ptstr)
{
	if (ptstr && *ptstr)
	{
		PTSTR pEnd = ptstr + UStrLen(ptstr);

		while ((pEnd > ptstr) && ((pEnd[-1] == ' ') || (pEnd[-1] == '\t')))
		{
			pEnd--;
			*pEnd = 0;
		}
	}
	return ptstr;
}

PTSTR UStrTrim(PTSTR ptstr)
{
	return UStrRightTrim(UStrLeftTrim(ptstr));
}

// Everything past the last backslash, or the whole path when there is none.
PTSTR UPathSplitName(PTSTR ptzPath)
{
	PTSTR pSlash;

	if (!ptzPath)
	{
		return NULL;
	}
	pSlash = UStrRChr(ptzPath, '\\');
	return pSlash ? (pSlash + 1) : ptzPath;
}

// Rewrites the two characters backslash-n into a real CR LF, in place. Both
// forms are two characters wide, which is what makes it safe to do where the
// text sits.
//
// One quirk is kept on purpose: the walk steps one character past the pair it
// just rewrote. That is what the text form does, and because the replacement is
// the same length the "skipped" character is the one already written over.
PTSTR USplitStr(PTSTR ptzStr)
{
	PTSTR pWalk = ptzStr;

	while (*pWalk)
	{
		if ((pWalk[0] == '\\') && (pWalk[1] == 'n'))
		{
			pWalk[0] = '\r';
			pWalk[1] = '\n';
			pWalk += 2;
		}
		pWalk++;
	}
	return ptzStr;
}