//////////////////////////////////////////////////////////////////////////////////////////////////
// Phosphor entry points.
//
// RunBytes is what LoadFile calls when a script starts with #!phos: the
// bytes are decoded, parsed and executed in one place. CheckSource is the half
// that runs on an ordinary Windows host (the "phos check" from the design
// document), so a typo in a startup script can be caught without booting a PE.
//////////////////////////////////////////////////////////////////////////////////////////////////
#include <set>

namespace Lang
{
	namespace
	{
		// A #!phos file is UTF-8. A file that is not valid UTF-8 falls back to
		// the ANSI code page, which is the same rule the text-command loader uses
		// and the one that keeps a GBK comment from turning into different
		// characters.
		String DecodeSource(const BYTE *pb, UINT uLen, UINT uBody)
		{
			const char *pc = (const char *) (pb + uBody);
			// MB_ERR_INVALID_CHARS is what makes the fallback below reachable:
			// without it an ill-formed sequence is replaced with U+FFFD and the
			// conversion "succeeds", so a GBK script would silently turn into
			// replacement characters instead of being read as ANSI.
			int nChars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pc, (INT) (uLen - uBody), NULL, 0);
			UINT uCode = CP_UTF8;
			String out;

			if (nChars <= 0)
			{
				uCode = CP_ACP;
				nChars = MultiByteToWideChar(CP_ACP, 0, pc, (INT) (uLen - uBody), NULL, 0);
			}
			if (nChars <= 0)
			{
				return out;
			}
			{
				std::vector<wchar_t> buf((size_t) nChars + 1, 0);
				MultiByteToWideChar(uCode, 0, pc, (INT) (uLen - uBody), buf.data(), nChars);
				out.assign(buf.data(), (size_t) nChars);
			}
			return out;
		}

		// Where the body starts, and which line that is. The shebang line and
		// the blank lines under it are not part of the program, but the line
		// numbers the lexer reports have to be the ones an editor shows, so the
		// count of skipped line breaks comes back with the offset.
		UINT MarkerBodyOffset(const BYTE *pb, UINT uLen, int *pFirstLine)
		{
			UINT p = 0;
			int line = 1;

			if ((uLen >= 3) && (pb[0] == 0xEF) && (pb[1] == 0xBB) && (pb[2] == 0xBF))
			{
				p = 3;
			}
			while ((p < uLen) && (pb[p] != L'\n') && (pb[p] != L'\r'))
			{
				p++;
			}
			while ((p < uLen) && ((pb[p] == L'\r') || (pb[p] == L'\n')))
			{
				if ((pb[p] == L'\r') && ((p + 1) < uLen) && (pb[p + 1] == L'\n'))
				{
					p++;
				}
				line++;
				p++;
			}
			if (pFirstLine)
			{
				*pFirstLine = line;
			}
			return p;
		}

		// The whole script, ready for the lexer: the marker is gone and the
		// lines it took are back as leading newlines, which the lexer counts and
		// the parser discards.
		String DecodeScript(const BYTE *pb, UINT uLen)
		{
			int firstLine = 1;
			String src = DecodeSource(pb, uLen, MarkerBodyOffset(pb, uLen, &firstLine));

			if (firstLine > 1)
			{
				src = String((size_t) (firstLine - 1), L'\n') + src;
			}
			return src;
		}

		bool ParseAll(const String& src, std::vector<StmtPtr>& program, String& err)
		{
			std::vector<Token> toks = Tokenize(src, err);

			if (!err.empty())
			{
				return false;
			}
			if (!ParseProgram(toks, program, err))
			{
				return false;
			}
			return true;
		}
	}

	BOOL IsMarker(const BYTE *pb, UINT uLen)
	{
		UINT p = 0;
		// The marker's length comes from the literal itself. Counting it by hand
		// beside the text is one rename away from being wrong, and a wrong count
		// means no script is recognised at all.
		const char c_marker[] = "#!phos";
		const UINT uMark = _countof(c_marker) - 1;

		if ((uLen >= 3) && (pb[0] == 0xEF) && (pb[1] == 0xBB) && (pb[2] == 0xBF))
		{
			p = 3;
		}
		if (uLen < p + uMark)
		{
			return FALSE;
		}
		for (UINT i = 0; i < uMark; i++)
		{
			if ((char) pb[p + i] != c_marker[i])
			{
				return FALSE;
			}
		}
		return TRUE;
	}

	HRESULT RunBytes(const BYTE *pb, UINT uLen, PCTSTR ptzPath)
	{
		String src = DecodeScript(pb, uLen);
		std::vector<StmtPtr> program;
		String err;
		Interp in;

		if (!ParseAll(src, program, err))
		{
			if (g_hXLog)
			{
				TCHAR tzBuf[MAX_STR];
				UStrPrint(tzBuf, TEXT("phos parse error: %s"), err.c_str());
				g_ctx.uLineNo = 0;
				SetLogs(tzBuf, E_FAIL);
			}
			return E_FAIL;
		}

		in.globals = std::shared_ptr<Env>(new Env(std::shared_ptr<Env>()));
		in.env = in.globals;
		if (ptzPath)
		{
			in.scriptPath = ptzPath;
			{
				String p = ptzPath;
				size_t slash = p.find_last_of(L"\\/");
				in.scriptDir = (slash == String::npos) ? String() : p.substr(0, slash);
			}
		}
		RegisterBuiltins(in);

		HRESULT hr = S_OK;

		try
		{
			RunProgram(in, program);
		}
		catch (Error& e)
		{
			if (g_hXLog)
			{
				TCHAR tzBuf[MAX_STR];
				UStrPrint(tzBuf, TEXT("phos error: line %d: %s"), e.line, e.message.c_str());
				g_ctx.uLineNo = (UINT) e.line;
				SetLogs(tzBuf, (HRESULT) e.code);
			}
			hr = (e.code != 0) ? (HRESULT) e.code : E_FAIL;
		}
		catch (ReturnSignal&)
		{
			// A top level return is just the end of the script.
		}
		catch (...)
		{
			// Anything the interpreter does not model by itself: an allocation
			// failure, an exception out of the standard library. Left alone it
			// reaches terminate(), which in a PE is the black screen with no
			// message at all. Note that this is not a catch-all for the machine:
			// under /EHsc a trap or an access violation is not a C++ exception
			// and still ends the process.
			if (g_hXLog)
			{
				TCHAR tzBuf[MAX_STR];

				UStrCopy(tzBuf, TEXT("phos: the interpreter stopped with an unmapped error"));
				SetLogs(tzBuf, E_FAIL);
			}
			hr = E_FAIL;
		}

		// Processes nobody awaited: closing the handle does not touch the
		// process, it only stops the script from leaking one handle per spawn.
		for (std::map<long long, HANDLE>::iterator it = in.procs.begin(); it != in.procs.end(); ++it)
		{
			CloseHandle(it->second);
		}
		in.procs.clear();
		return hr;
	}

	// --- the host side check ---------------------------------------------------------
	namespace
	{
		struct Resolver
		{
			std::vector<std::set<String> > scopes;
			String err;

			Resolver()
			{
				scopes.push_back(std::set<String>());
				scopes.back().insert(L"path");		// supplied by RegisterBuiltins
			}

			void Declare(const String& n) { scopes.back().insert(n); }

			bool Declared(const String& n) const
			{
				for (size_t i = scopes.size(); i > 0; i--)
				{
					if (scopes[i - 1].count(n))
					{
						return true;
					}
				}
				return false;
			}

			bool Fail(const String& m, int line)
			{
				if (err.empty())
				{
					TCHAR tzBuf[512];
					UStrPrint(tzBuf, TEXT("line %d: %s"), line, m.c_str());
					err = tzBuf;
				}
				return false;
			}

			bool Expr(const ExprPtr& e)
			{
				switch (e->kind)
				{
				case EX_VAR:
					if (!Declared(e->name))
					{
						return Fail(String(L"undefined name '") + e->name + L"'", e->line);
					}
					return true;
				case EX_CALL:
					{
						if (!FindHost(e->name) && !Declared(e->name))
						{
							return Fail(String(L"unknown function '") + e->name + L"'", e->line);
						}
						for (size_t i = 0; i < e->items.size(); i++)
						{
							if (!Expr(e->items[i]))
							{
								return false;
							}
						}
						return true;
					}
				case EX_FUNC:
					{
						scopes.push_back(std::set<String>());
						for (size_t i = 0; i < e->params.size(); i++)
						{
							Declare(e->params[i]);
						}
						for (size_t i = 0; i < e->fnBody.size(); i++)
						{
							if (!Stmt_(e->fnBody[i]))
							{
								return false;
							}
						}
						scopes.pop_back();
						return true;
					}
				default:
					for (size_t i = 0; i < e->items.size(); i++)
					{
						if (!Expr(e->items[i]))
						{
							return false;
						}
					}
					return true;
				}
			}

			bool Stmt_(const StmtPtr& s)
			{
				switch (s->kind)
				{
				case ST_LET:
					if (!Expr(s->expr)) return false;
					Declare(s->name);
					return true;
				case ST_ASSIGN:
					if (!Expr(s->expr)) return false;		// target: name/index, must exist
					return Expr(s->expr2);
				case ST_EXPR:
					return Expr(s->expr);
				case ST_IF:
					if (!Expr(s->expr)) return false;
					scopes.push_back(std::set<String>());
					for (size_t i = 0; i < s->body.size(); i++) { if (!Stmt_(s->body[i])) return false; }
					scopes.pop_back();
					scopes.push_back(std::set<String>());
					for (size_t i = 0; i < s->elseBody.size(); i++) { if (!Stmt_(s->elseBody[i])) return false; }
					scopes.pop_back();
					return true;
				case ST_WHILE:
					if (!Expr(s->expr)) return false;
					scopes.push_back(std::set<String>());
					for (size_t i = 0; i < s->body.size(); i++) { if (!Stmt_(s->body[i])) return false; }
					scopes.pop_back();
					return true;
				case ST_FOR:
					if (!Expr(s->expr)) return false;
					scopes.push_back(std::set<String>());
					Declare(s->name);
					for (size_t i = 0; i < s->body.size(); i++) { if (!Stmt_(s->body[i])) return false; }
					scopes.pop_back();
					return true;
				case ST_RETURN:
					return s->expr ? Expr(s->expr) : true;
				case ST_FUNC:
					Declare(s->name);
					scopes.push_back(std::set<String>());
					for (size_t i = 0; i < s->params.size(); i++) { Declare(s->params[i]); }
					for (size_t i = 0; i < s->body.size(); i++) { if (!Stmt_(s->body[i])) return false; }
					scopes.pop_back();
					return true;
				case ST_TRY:
					scopes.push_back(std::set<String>());
					for (size_t i = 0; i < s->body.size(); i++) { if (!Stmt_(s->body[i])) return false; }
					scopes.pop_back();
					scopes.push_back(std::set<String>());
					Declare(s->name);
					for (size_t i = 0; i < s->catchBody->body.size(); i++) { if (!Stmt_(s->catchBody->body[i])) return false; }
					scopes.pop_back();
					return true;
				}
				return true;
			}

			bool Run(const std::vector<StmtPtr>& program)
			{
				for (size_t i = 0; i < program.size(); i++)
				{
					if (!Stmt_(program[i]))
					{
						return false;
					}
				}
				return true;
			}
		};
	}	// namespace

	bool CheckSource(const String& src, String& errOut)
	{
		std::vector<StmtPtr> program;
		String err;
		Resolver r;

		if (!ParseAll(src, program, err))
		{
			errOut = err;
			return false;
		}
		if (!r.Run(program))
		{
			errOut = r.err;
			return false;
		}
		errOut.clear();
		return true;
	}

	bool CheckFile(PCTSTR ptzPath, String& errOut)
	{
		UINT uSize = -1;
		PBYTE pb = (PBYTE) UFileLoad((PTSTR) ptzPath, NULL, &uSize);
		bool ok;

		if (!pb)
		{
			errOut = L"cannot read file";
			return false;
		}
		{
			String src = DecodeScript(pb, uSize);
			ok = CheckSource(src, errOut);
		}
		UMemFree(pb);
		return ok;
	}
}
