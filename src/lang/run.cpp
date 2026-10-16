//////////////////////////////////////////////////////////////////////////////////////////////////
// Phosphor entry points.
//
// RunBytes is what LoadFile calls when a script starts with #!phos: the
// bytes are decoded, parsed and executed in one place. CheckSource is the half
// that runs on an ordinary Windows host (the "phos check" from the design
// document), so a typo in a startup script can be caught without booting a PE.
//////////////////////////////////////////////////////////////////////////////////////////////////
#include <set>
#include "../core/encoding.h"

namespace Lang
{
	namespace
	{
		// The whole script, ready for the lexer: the marker is gone and the
		// lines it took are back as leading newlines, which the lexer counts and
		// the parser discards.
		//
		// Decode first, then work on text. The byte version had to know about a
		// UTF-8 byte-order mark and nothing else -- which is why a script saved
		// as UTF-16 by an editor was not recognised as a script at all, and
		// quietly did nothing. Decoding first means one implementation for every
		// encoding the decoder understands.
		String DecodeScript(const BYTE *pb, UINT uLen)
		{
			String src = DecodeTextBytes(pb, uLen);
			size_t at = 0;
			int skipped = 1;

			// The marker's own line.
			while ((at < src.size()) && (src[at] != L'\r') && (src[at] != L'\n'))
			{
				at++;
			}
			// And the blank lines under it, counted so the lexer's first line is
			// the line an editor shows.
			while (at < src.size())
			{
				if (src[at] == L'\r')
				{
					at++;
					if ((at < src.size()) && (src[at] == L'\n'))
					{
						at++;
					}
				}
				else if (src[at] == L'\n')
				{
					at++;
				}
				else
				{
					break;
				}
				skipped++;
			}
			return String((size_t) (skipped - 1), L'\n') + src.substr(at);
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
		// Decode first, for the same reason DecodeScript does: the marker is
		// text, and comparing bytes only recognises the encodings somebody
		// remembered. This understood a UTF-8 byte-order mark and nothing else,
		// so a script an editor saved as UTF-16 was not recognised as one.
		//
		// The length comes from the literal. It used to be spelled "9" beside
		// the text, which held only until the marker was renamed: the previous
		// spelling was nine characters, "#!phos" is six, and the loop went on
		// comparing nine.
		const TCHAR c_marker[] = TEXT("#!phos");
		const size_t uMark = _countof(c_marker) - 1;
		String src = DecodeTextBytes(pb, uLen);

		if (src.size() < uMark)
		{
			return FALSE;
		}
		for (size_t i = 0; i < uMark; i++)
		{
			if (src[i] != c_marker[i])
			{
				return FALSE;
			}
		}
		return TRUE;
	}

	// Declared here and defined below, after Resolver: both entry points go
	// through this one function so the checker and the runner cannot drift
	// apart about what counts as an acceptable program.
	bool ResolveProgram(const std::vector<StmtPtr>& program, String *pErr);

	HRESULT RunBytes(const BYTE *pb, UINT uLen, PCTSTR ptzPath)
	{
		String src = DecodeScript(pb, uLen);
		std::vector<StmtPtr> program;
		String err;
		// On the heap, and with a reference to itself, so that a closure bound
		// to a hotkey can keep the interpreter -- and the scope it captured --
		// alive after this call has returned. A stack interpreter would be gone
		// by the time a key is pressed.
		std::shared_ptr<Interp> pIn(new Interp());
		Interp& in = *pIn;

		pIn->self = pIn;

		// The crash record says which script it was in. The pointer is into the
		// caller's path string, which outlives this run.
		CrashSetScript(ptzPath);

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

		// Resolve before running anything.
		//
		// The checker has always done this; the run path did not, so a
		// misspelled host verb was only discovered when execution reached that
		// line -- with everything above it already done. In a boot chain that is
		// half a boot and then a stop, and the two entry points could disagree
		// about whether the same file was acceptable. Nothing executes until the
		// whole program has been resolved.
		{
			String resolveErr;

			if (!ResolveProgram(program, &resolveErr))
			{
				if (g_hXLog)
				{
					TCHAR tzBuf[MAX_STR];
					UStrPrint(tzBuf, TEXT("phos resolve error: %s"), resolveErr.c_str());
					g_ctx.uLineNo = 0;
					SetLogs(tzBuf, E_FAIL);
				}
				return E_FAIL;
			}
		}

		in.globals = std::shared_ptr<Env>(new Env(std::shared_ptr<Env>()));
		in.env = in.globals;
		// `--dry-run` from the command line, carried in from the process.
		in.dryRun = g_bDryRun ? TRUE : FALSE;
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
		catch (GuardSignal& g)
		{
			// The same shape as the Error branch above, because to the person
			// reading `init-error.txt` this is the same kind of stop: a line, a
			// message, a code. What differs is that the language's `catch` could
			// not have taken it -- see GuardSignal in runtime.h.
			if (g_hXLog)
			{
				TCHAR tzBuf[MAX_STR];
				UStrPrint(tzBuf, TEXT("phos error: line %d: %s"), g.line, g.message.c_str());
				g_ctx.uLineNo = (UINT) g.line;
				SetLogs(tzBuf, (HRESULT) g.code);
			}
			hr = (g.code != 0) ? (HRESULT) g.code : E_FAIL;
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
		// The names a function body reads that are not its own parameters or
		// locals: exactly the values a closure captures (SPEC-4). A function
		// nested inside the body is its own closure, so what it needs that this
		// body does not bind has to be captured here too, or this closure would
		// have nothing to hand on.
		void FreeExpr(const ExprPtr& e, std::set<String>& bound, std::set<String>& free);
		void FreeStmt(const StmtPtr& s, std::set<String>& bound, std::set<String>& free);

		void FreeExpr(const ExprPtr& e, std::set<String>& bound, std::set<String>& free)
		{
			if (!e)
			{
				return;
			}
			if ((e->kind == EX_VAR) || (e->kind == EX_CALL))
			{
				if (!bound.count(e->name))
				{
					free.insert(e->name);
				}
			}
			if (e->kind == EX_FUNC)
			{
				std::set<String> inner;
				for (size_t i = 0; i < e->params.size(); i++)
				{
					inner.insert(e->params[i]);
				}
				std::set<String> subFree;
				for (size_t i = 0; i < e->fnBody.size(); i++)
				{
					FreeStmt(e->fnBody[i], inner, subFree);
				}
				for (std::set<String>::iterator it = subFree.begin(); it != subFree.end(); ++it)
				{
					if (!bound.count(*it))
					{
						free.insert(*it);
					}
				}
				return;
			}
			for (size_t i = 0; i < e->items.size(); i++)
			{
				FreeExpr(e->items[i], bound, free);
			}
		}

		void FreeStmt(const StmtPtr& s, std::set<String>& bound, std::set<String>& free)
		{
			switch (s->kind)
			{
			case ST_LET:
				FreeExpr(s->expr, bound, free);
				bound.insert(s->name);
				return;
			case ST_ASSIGN:
				FreeExpr(s->expr, bound, free);
				FreeExpr(s->expr2, bound, free);
				return;
			case ST_EXPR:
				FreeExpr(s->expr, bound, free);
				return;
			case ST_IF:
				FreeExpr(s->expr, bound, free);
				for (size_t i = 0; i < s->body.size(); i++) { FreeStmt(s->body[i], bound, free); }
				for (size_t i = 0; i < s->elseBody.size(); i++) { FreeStmt(s->elseBody[i], bound, free); }
				return;
			case ST_WHILE:
				FreeExpr(s->expr, bound, free);
				for (size_t i = 0; i < s->body.size(); i++) { FreeStmt(s->body[i], bound, free); }
				return;
			case ST_FOR:
				FreeExpr(s->expr, bound, free);
				bound.insert(s->name);
				for (size_t i = 0; i < s->body.size(); i++) { FreeStmt(s->body[i], bound, free); }
				return;
			case ST_RETURN:
				if (s->expr) { FreeExpr(s->expr, bound, free); }
				return;
			case ST_FUNC:
				{
					bound.insert(s->name);
					std::set<String> inner;
					for (size_t i = 0; i < s->params.size(); i++) { inner.insert(s->params[i]); }
					std::set<String> subFree;
					for (size_t i = 0; i < s->body.size(); i++) { FreeStmt(s->body[i], inner, subFree); }
					for (std::set<String>::iterator it = subFree.begin(); it != subFree.end(); ++it)
					{
						if (!bound.count(*it)) { free.insert(*it); }
					}
				}
				return;
			case ST_TRY:
				for (size_t i = 0; i < s->body.size(); i++) { FreeStmt(s->body[i], bound, free); }
				bound.insert(s->name);
				for (size_t i = 0; i < s->catchBody->body.size(); i++) { FreeStmt(s->catchBody->body[i], bound, free); }
				return;
			case ST_BREAK:
			case ST_CONTINUE:
				return;		// no names, nothing to capture
			}
		}

		// The capture list stored on a function node. std::set walks in order,
		// so the list does not depend on the order the body happened to be in.
		std::vector<String> CapturesOf(const std::vector<String>& params, const std::vector<StmtPtr>& body)
		{
			std::set<String> bound;
			for (size_t i = 0; i < params.size(); i++)
			{
				bound.insert(params[i]);
			}
			std::set<String> free;
			for (size_t i = 0; i < body.size(); i++)
			{
				FreeStmt(body[i], bound, free);
			}
			return std::vector<String>(free.begin(), free.end());
		}

		struct Resolver
		{
			std::vector<std::set<String> > scopes;
			// One frame per function entered. `base` is where its own scopes
			// begin in `scopes`; `closure` says whether it captures by value (an
			// anonymous or nested function) rather than being a module table
			// entry, which is what decides whether writing to a name that lives
			// below the frame is legal.
			struct Frame { size_t base; bool closure; };
			std::vector<Frame> fns;
			String err;
			// How many loops enclose the statement being resolved. `break` and
			// `continue` are only meaningful inside one, and a signal that
			// escapes to the top would leave RunBytes through a path nothing
			// catches.
			int loops;
			// How deep the stack is while resolving the script's top level:
			// scopes[0] is the host's own names and scopes[1] is the script's,
			// so every push after this one is a block or a function. Written
			// down rather than spelled as a literal, because "is this a top
			// level function" is the difference between a module table entry
			// and a closure.
			size_t topDepth;

			Resolver() : loops(0)
			{
				// The host's own names get a scope of their own, below the
				// script's. `RegisterBuiltins` supplies `path` and `script`; a
				// script may still shadow one on purpose -- `let script = ...`
				// is in the shipped cases -- and the duplicate check below must
				// not count a shadow as a re-declaration.
				scopes.push_back(std::set<String>());
				scopes.back().insert(L"path");
				scopes.back().insert(L"script");
				scopes.push_back(std::set<String>());	// the script's top level
				topDepth = scopes.size();
			}

			void Declare(const String& n) { scopes.back().insert(n); }

			// `let` is a declaration, so saying it twice in one scope is a
			// mistake the load can see: the second one silently won, and nothing
			// in the trace said which of the two the rest of the script meant.
			// The other places that bind a name (parameters, `for`, `catch`) go
			// through Declare and may shadow -- a loop variable with the same
			// name as an earlier one is a different binding in a different
			// statement, not a second declaration of one name.
			bool DeclareLet(const String& n, int line)
			{
				if (scopes.back().count(n))
				{
					return Fail(String(L"'") + n + L"' is already declared in this scope", line);
				}
				scopes.back().insert(n);
				return true;
			}

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

			// Is `n` declared inside the function being resolved, rather than a
			// scope it captured from?
			bool LocalToFunction(const String& n) const
			{
				if (fns.empty())
				{
					return true;
				}
				for (size_t i = scopes.size(); i > fns.back().base; i--)
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
						// An anonymous function is a closure: record what it
						// captures, then resolve its body with assignment to a
						// captured name refused.
						e->captures = CapturesOf(e->params, e->fnBody);
						Frame f; f.base = scopes.size(); f.closure = true;
						fns.push_back(f);
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
						fns.pop_back();
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
					if (!DeclareLet(s->name, s->line)) return false;
					return true;
				case ST_ASSIGN:
					if (!Expr(s->expr)) return false;		// target: name/index, must exist
					// SPEC-4: a closure captures by value, so it has no cell to
					// write a captured name back to. Assigning to one is refused
					// before the program runs, rather than silently lost when the
					// closure is called.
					if (!fns.empty() && fns.back().closure && (s->expr->kind == EX_VAR)
						&& !LocalToFunction(s->expr->name))
					{
						return Fail(String(L"cannot assign to captured name '") + s->expr->name
							+ L"' (a closure captures by value)", s->line);
					}
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
					loops++;
					for (size_t i = 0; i < s->body.size(); i++) { if (!Stmt_(s->body[i])) return false; }
					loops--;
					scopes.pop_back();
					return true;
				case ST_FOR:
					if (!Expr(s->expr)) return false;
					scopes.push_back(std::set<String>());
					Declare(s->name);
					loops++;
					for (size_t i = 0; i < s->body.size(); i++) { if (!Stmt_(s->body[i])) return false; }
					loops--;
					scopes.pop_back();
					return true;
				case ST_RETURN:
					return s->expr ? Expr(s->expr) : true;
				case ST_BREAK:
					return (loops > 0) ? true : Fail(L"'break' outside a loop", s->line);
				case ST_CONTINUE:
					return (loops > 0) ? true : Fail(L"'continue' outside a loop", s->line);
				case ST_FUNC:
					{
						// Declared at the top level it is a module table entry:
						// looked up by name, so it can call one written later and
						// may write a module global. Declared inside a body it is
						// a closure and captures what it uses.
						bool moduleFn = (scopes.size() == topDepth);

						// A function the language can never reach is worse than a
						// name that is refused: `fn len(x) { ... }` would be
						// declared, and every call would go to the host's own
						// `len` instead (EX_CALL looks in the verb table first),
						// so the body would never run and nothing would say why.
						if (HostHasVerb(s->name))
						{
							return Fail(String(L"'") + s->name + L"' is a host verb, so a function"
								L" of that name would never be called", s->line);
						}
						Declare(s->name);
						if (!moduleFn)
						{
							s->captures = CapturesOf(s->params, s->body);
						}
						Frame f; f.base = scopes.size(); f.closure = !moduleFn;
						fns.push_back(f);
						scopes.push_back(std::set<String>());
						for (size_t i = 0; i < s->params.size(); i++) { Declare(s->params[i]); }
						for (size_t i = 0; i < s->body.size(); i++) { if (!Stmt_(s->body[i])) return false; }
						scopes.pop_back();
						fns.pop_back();
						return true;
					}
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
				// Top level functions first: they are the module's function
				// table, so a call to one written further down the file resolves.
				// Nothing else is hoisted; a variable still has to be declared
				// before it is read.
				for (size_t i = 0; i < program.size(); i++)
				{
					if (program[i]->kind == ST_FUNC)
					{
						Declare(program[i]->name);
					}
				}
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

	// The one place that runs the resolver. Both entry points go through it, so
	// the checker and the runner cannot drift apart about what they accept.
	bool ResolveProgram(const std::vector<StmtPtr>& program, String *pErr)
	{
		Resolver r;

		if (!r.Run(program))
		{
			if (pErr)
			{
				*pErr = r.err;
			}
			return false;
		}
		return true;
	}

	bool CheckSource(const String& src, String& errOut)
	{
		std::vector<StmtPtr> program;
		String err;

		if (!ParseAll(src, program, err))
		{
			errOut = err;
			return false;
		}
		if (!ResolveProgram(program, &errOut))
		{
			return false;
		}
		errOut.clear();
		return true;
	}

	bool CheckFile(PCTSTR ptzPath, String& errOut)
	{
		UINT uSize = (UINT) -1;
		PBYTE pb = (PBYTE) UFileLoad((PTSTR) ptzPath, &uSize);
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
