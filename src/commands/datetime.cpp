//
// DATE: the current date and time.
//
//   DATE [<&var>] [<subitem>]
//
// The default value is the documented shape - "2011-8-8|5|20:8:8":
// year, month and day unpadded, the weekday with 1 = Monday, then the time.
// A subitem asks for one field on its own. The switches that ask for a
// conversion, a high-resolution counter or the space-separated forms are not
// implemented and refuse; the format has to be measured before it is written.
//
HRESULT Date(CoreContext& ctx, PTSTR ptzCmd)
{
	SYSTEMTIME st;
	TCHAR tzOut[MAX_STR];
	TCHAR tzSub[MAX_NAME];
	PTSTR ptzVar;
	PTSTR ptzSub;
	UINT uWeekday;

	if (ptzCmd[0] == '-')
	{
		return E_INVALIDARG;
	}
	ptzSub = UStrChr(ptzCmd, ' ');
	if (ptzSub)
	{
		*ptzSub++ = 0;
		UStrCopy(tzSub, UStrTrim(ptzSub));
	}
	else
	{
		tzSub[0] = 0;
	}
	ptzVar = UStrTrim(ptzCmd);
	if (!ptzVar[0])
	{
		ptzVar = TEXT("CurDate");
	}

	GetLocalTime(&st);
	uWeekday = (((UINT) st.wDayOfWeek + 6) % 7) + 1;      // 1 = Monday

	if (!tzSub[0])
	{
		UStrPrint(tzOut, TEXT("%u-%u-%u|%u|%u:%u:%u"),
			st.wYear, st.wMonth, st.wDay, uWeekday,
			st.wHour, st.wMinute, st.wSecond);
	}
	else if (!UStrCmpI(tzSub, TEXT("y")))
	{
		UStrPrint(tzOut, TEXT("%u"), st.wYear);
	}
	else if (!UStrCmpI(tzSub, TEXT("mon")))
	{
		UStrPrint(tzOut, TEXT("%u"), st.wMonth);
	}
	else if (!UStrCmpI(tzSub, TEXT("d")))
	{
		UStrPrint(tzOut, TEXT("%u"), st.wDay);
	}
	else if (!UStrCmpI(tzSub, TEXT("w")))
	{
		UStrPrint(tzOut, TEXT("%u"), uWeekday);
	}
	else if (!UStrCmpI(tzSub, TEXT("h")))
	{
		UStrPrint(tzOut, TEXT("%u"), st.wHour);
	}
	else if (!UStrCmpI(tzSub, TEXT("min")))
	{
		UStrPrint(tzOut, TEXT("%u"), st.wMinute);
	}
	else if (!UStrCmpI(tzSub, TEXT("s")))
	{
		UStrPrint(tzOut, TEXT("%u"), st.wSecond);
	}
	else if (!UStrCmpI(tzSub, TEXT("ms")))
	{
		UStrPrint(tzOut, TEXT("%u"), st.wMilliseconds);
	}
	else
	{
		return E_INVALIDARG;
	}
	return StoreResult(ptzVar, tzOut) ? S_OK : S_FALSE;
}
