//////////////////////////////////////////////////////////////////////////////////////////////////
// PE variables
//
// A second set of variables is kept next to the process environment, and
// the prefix decides which one is meant:
//
//   %Name%      the process environment (Win32, inherited by child processes)
//   %&Name%     a PE variable: the nearest scope wins, and one is created in the
//               current scope when nothing matches
//   %&&Name%    a PE variable in the current scope only
//   %&::Name%   a PE variable in the global scope
//
// SET always writes a PE variable; ENVI writes the environment unless
// ENVI^ ForceLocal=1 says otherwise. PE expansion runs before the Win32 call in
// Command(), so the two syntaxes never have to know about each other.
//////////////////////////////////////////////////////////////////////////////////////////////////

#include <map>
#include <string>
#include <vector>
#include <stdio.h>

namespace PeVar
{
	typedef std::basic_string<TCHAR> String;

	struct LessIgnoreCase
	{
		bool operator()(const String& a, const String& b) const
		{
			return UStrCmpI(a.c_str(), b.c_str()) < 0;
		}
	};

	typedef std::map<String, String, LessIgnoreCase> Table;

	static Table g_Global;
	static std::vector<Table> g_Scopes;
	static BOOL g_bForceLocal = FALSE;
	static BOOL g_bEnviMode = FALSE;

	static Table& CurrentScope()
	{
		if (g_Scopes.empty())
		{
			g_Scopes.push_back(Table());
		}
		return g_Scopes.back();
	}

	enum Scope { SCOPE_NEAREST, SCOPE_LOCAL, SCOPE_GLOBAL };

	// Split "&::a" / "&&a" / "&a" into the scope it names and the bare key.
	static Scope Classify(PCTSTR ptzName, PCTSTR& ptzKey)
	{
		if ((ptzName[0] == '&') && (ptzName[1] == ':') && (ptzName[2] == ':'))
		{
			ptzKey = ptzName + 3;
			return SCOPE_GLOBAL;
		}
		if ((ptzName[0] == '&') && (ptzName[1] == '&'))
		{
			ptzKey = ptzName + 2;
			return SCOPE_LOCAL;
		}
		ptzKey = ptzName + 1;
		return SCOPE_NEAREST;
	}

	static String * FindNearest(PCTSTR ptzKey)
	{
		String key(ptzKey);
		for (size_t i = g_Scopes.size(); i > 0; i--)
		{
			Table::iterator it = g_Scopes[i - 1].find(key);
			if (it != g_Scopes[i - 1].end())
			{
				return &it->second;
			}
		}
		Table::iterator it = g_Global.find(key);
		return (it != g_Global.end()) ? &it->second : NULL;
	}

	BOOL IsPeName(PCTSTR ptzName)
	{
		return (ptzName && (ptzName[0] == '&'));
	}

	BOOL Lookup(PCTSTR ptzName, String& value)
	{
		PCTSTR ptzKey = NULL;
		Scope scope;

		if (!IsPeName(ptzName))
		{
			return FALSE;
		}
		scope = Classify(ptzName, ptzKey);

		if (scope == SCOPE_GLOBAL)
		{
			Table::iterator it = g_Global.find(String(ptzKey));
			if (it == g_Global.end())
			{
				return FALSE;
			}
			value = it->second;
			return TRUE;
		}
		if (scope == SCOPE_LOCAL)
		{
			Table& table = CurrentScope();
			Table::iterator it = table.find(String(ptzKey));
			if (it == table.end())
			{
				return FALSE;
			}
			value = it->second;
			return TRUE;
		}

		String * pFound = FindNearest(ptzKey);
		if (!pFound)
		{
			return FALSE;
		}
		value = *pFound;
		return TRUE;
	}

	// The value of a PE variable, copied out. FALSE when it is not set, which
	// some commands have to tell apart from "set to an empty string".
	BOOL Get(PCTSTR ptzName, PTSTR ptzOut, UINT uOut)
	{
		String value;

		if (!Lookup(ptzName, value))
		{
			return FALSE;
		}
		UStrCopyN(ptzOut, (PTSTR) value.c_str(), uOut);
		return TRUE;
	}

	BOOL Set(PCTSTR ptzName, PCTSTR ptzValue)
	{
		PCTSTR ptzKey = NULL;
		Scope scope;

		if (!IsPeName(ptzName))
		{
			return FALSE;
		}
		scope = Classify(ptzName, ptzKey);

		if (scope == SCOPE_GLOBAL)
		{
			g_Global[String(ptzKey)] = ptzValue;
			return TRUE;
		}
		if (scope == SCOPE_LOCAL)
		{
			CurrentScope()[String(ptzKey)] = ptzValue;
			return TRUE;
		}

		String * pFound = FindNearest(ptzKey);
		if (pFound)
		{
			*pFound = ptzValue;
		}
		else
		{
			// Nearest with nothing to match creates a local. That is what makes
			// "&a" the "just give me a variable" form.
			CurrentScope()[String(ptzKey)] = ptzValue;
		}
		return TRUE;
	}

	VOID PushScope()
	{
		g_Scopes.push_back(Table());
	}

	VOID PopScope()
	{
		if (!g_Scopes.empty())
		{
			g_Scopes.pop_back();
		}
	}

	// Scope lifetime as an object: a sub process, or a whole loaded script, is a
	// scope, and every way out of it has to pop.
	struct ScopeGuard
	{
		ScopeGuard() { PushScope(); }
		~ScopeGuard() { PopScope(); }
	};

	BOOL ForceLocal() { return g_bForceLocal; }
	VOID SetForceLocal(BOOL bValue) { g_bForceLocal = bValue; }
	BOOL EnviMode() { return g_bEnviMode; }
	VOID SetEnviMode(BOOL bValue) { g_bEnviMode = bValue; }

	// 63 bits, non-negative, different on every call. xorshift64* seeded from
	// the clock, the thread and a constant: the point is that a script cannot
	// predict it, not that it is cryptographically strong.
	UINT64 NextRandom()
	{
		static UINT64 s_state = 0;

		if (!s_state)
		{
			LARGE_INTEGER liTick;
			QueryPerformanceCounter(&liTick);
			s_state = (UINT64) liTick.QuadPart
				^ (((UINT64) GetTickCount()) << 32)
				^ ((UINT64) GetCurrentThreadId())
				^ 0x9E3779B97F4A7C15ULL;
			if (!s_state)
			{
				s_state = 0x2545F4914F6CDD1DULL;
			}
		}
		s_state ^= s_state >> 12;
		s_state ^= s_state << 25;
		s_state ^= s_state >> 27;
		return (s_state * 0x2545F4914F6CDD1DULL) & 0x7FFFFFFFFFFFFFFFULL;
	}

	// Replace %&name% / %&&name% / %&::name% into ptzOut. A name that is not set
	// is left alone, which is what the Win32 call does with an unknown %Name%.
	// %RANDOM% is answered here too: it is one of this program's own variables
	// rather than an environment variable, and every expansion supplies a fresh
	// non-negative 63-bit integer.
	VOID Expand(PTSTR ptzOut, UINT uOut, PCTSTR ptzIn)
	{
		String out;

		for (PCTSTR p = ptzIn; *p; )
		{
			if (p[0] == '%')
			{
				PCTSTR pEnd = UStrChr(p + 1, '%');
				if (pEnd)
				{
					PCTSTR pName = p + 1;
					if (*pName == '&')
					{
						String name(pName, (size_t) (pEnd - pName));
						String value;
						// A PE variable that is not set expands to nothing. That
						// is the documented behaviour and what the text form
						// does; the Win32 half is the one that leaves an unknown
						// %Name% alone, and the two must not be mixed up.
						if (Lookup(name.c_str(), value))
						{
							out += value;
						}
						p = pEnd + 1;
						continue;
					}
					if (((size_t) (pEnd - pName) == 6) &&
						(!UStrCmpNI(pName, TEXT("RANDOM"), 6)))
					{
						TCHAR tzRandom[32];
						_snwprintf_s(tzRandom, _NumOf(tzRandom), _TRUNCATE,
							TEXT("%I64u"), NextRandom());
						out += tzRandom;
						p = pEnd + 1;
						continue;
					}
				}
			}
			out += *p++;
		}
		UStrCopyN(ptzOut, (PTSTR) out.c_str(), uOut);
	}
}
