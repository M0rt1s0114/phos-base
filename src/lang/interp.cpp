//////////////////////////////////////////////////////////////////////////////////////////////////
// Phosphor interpreter.
//
// A tree walking evaluator over the parser's output. Values copy on assignment,
// only false and nil are falsy, and a failing host call raises unless the script
// wraps it in try.
//
// There is a step ceiling. It is not a language feature, it is a WinPE
// seatbelt: "while true { }" in a boot script must not leave the machine
// hanging with a black screen, so the interpreter stops the script and reports
// it instead.
//////////////////////////////////////////////////////////////////////////////////////////////////
namespace Lang
{
	// The most negative 64-bit value. Spelled here rather than taken from
	// <limits.h>, which this tree does not include.
	const long long c_i64Min = (-9223372036854775807LL - 1LL);

	Value CopyValue(const Value& v)
	{
		Value out(v);

		if ((v.kind == VK_LIST) && v.list)
		{
			out.list = ListPtr(new std::vector<Value>());
			out.list->reserve(v.list->size());
			for (size_t i = 0; i < v.list->size(); i++)
			{
				out.list->push_back(CopyValue((*v.list)[i]));
			}
		}
		else if ((v.kind == VK_RECORD) && v.rec)
		{
			out.rec = RecordPtr(new std::vector<std::pair<String, Value> >());
			out.rec->reserve(v.rec->size());
			for (size_t i = 0; i < v.rec->size(); i++)
			{
				out.rec->push_back(std::make_pair((*v.rec)[i].first, CopyValue((*v.rec)[i].second)));
			}
		}
		return out;
	}

	String ToText(const Value& v)
	{
		switch (v.kind)
		{
		case VK_NIL:  return L"nil";
		case VK_BOOL: return v.b ? L"true" : L"false";
		case VK_INT:
			{
				wchar_t buf[32];
				_snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%I64d", v.i);
				return buf;
			}
		case VK_STR:  return v.s;
		case VK_LIST: return L"(list)";
		case VK_RECORD: return L"(record)";
		case VK_FUNC: return L"(fn)";
		}
		return L"";
	}

	String ToDisplay(const Value& v)
	{
		if (v.kind == VK_STR)
		{
			return v.s;
		}
		return ToText(v);
	}

	bool Truthy(const Value& v)
	{
		if (v.kind == VK_NIL)
		{
			return false;
		}
		if (v.kind == VK_BOOL)
		{
			return v.b;
		}
		return true;
	}

	bool ValueEquals(const Value& a, const Value& b)
	{
		if (a.kind != b.kind)
		{
			return false;
		}
		switch (a.kind)
		{
		case VK_NIL:    return true;
		case VK_BOOL:   return a.b == b.b;
		case VK_INT:    return a.i == b.i;
		case VK_STR:    return a.s == b.s;
		case VK_LIST:   return a.list == b.list;
		case VK_RECORD: return a.rec == b.rec;
		case VK_FUNC:   return a.fn == b.fn;
		}
		return false;
	}

	namespace
	{
		typedef std::shared_ptr<Env> EnvPtr;

		Value *LookupVar(const EnvPtr& env, const String& name)
		{
			for (EnvPtr e = env; e; e = e->parent)
			{
				std::map<String, Value>::iterator it = e->vars.find(name);
				if (it != e->vars.end())
				{
					return &it->second;
				}
			}
			return NULL;
		}

		void Tick(Interp& in, int line)
		{
			if (++in.steps > in.stepLimit)
			{
				throw Error(0x8000FFFF, L"step limit reached (runaway loop?)", line);
			}
		}

		Value EvalExpr(Interp& in, const ExprPtr& e, const EnvPtr& env);
		void  ExecStmt(Interp& in, const StmtPtr& s, const EnvPtr& env);
		void  ExecBlock(Interp& in, const std::vector<StmtPtr>& body, const EnvPtr& env);

		long long AsInt(const Value& v, int line, const wchar_t *what)
		{
			if (v.kind != VK_INT)
			{
				throw Error(0x80070057, String(L"expected an integer for ") + what, line);
			}
			return v.i;
		}

		String AsStr(const Value& v, int line, const wchar_t *what)
		{
			if (v.kind != VK_STR)
			{
				throw Error(0x80070057, String(L"expected a string for ") + what, line);
			}
			return v.s;
		}

		Value CallFunc(Interp& in, const FuncPtr& fn, std::vector<Value>& args, int line)
		{
			EnvPtr call(new Env(fn->env));

			if (args.size() != fn->params.size())
			{
				TCHAR tzBuf[128];
				UStrPrint(tzBuf, TEXT("expected %u arguments, got %u"), (UINT) fn->params.size(), (UINT) args.size());
				throw Error(0x80070057, tzBuf, line);
			}

			// The ceiling is what turns runaway recursion into a script error
			// instead of a stack the process never comes back from.
			if (in.depth >= in.depthLimit)
			{
				throw Error(0x8000FFFF, L"call depth limit reached (runaway recursion?)", line);
			}
			struct DepthGuard
			{
				Interp& in;
				explicit DepthGuard(Interp& target) : in(target) { in.depth++; }
				~DepthGuard() { in.depth--; }
			} guard(in);

			// Arguments are copied into the callee, so a list passed in cannot be
			// written through from the other side.
			for (size_t i = 0; i < fn->params.size(); i++)
			{
				call->vars[fn->params[i]] = CopyValue(args[i]);
			}

			try
			{
				ExecBlock(in, fn->body, call);
			}
			catch (ReturnSignal& r)
			{
				return r.value;
			}
			return Value();
		}

		Value EvalBinary(Interp& in, const ExprPtr& e, const EnvPtr& env)
		{
			Value a = EvalExpr(in, e->items[0], env);

			// Short circuit: && and || must not evaluate the right hand side
			// unless they have to, or "x != nil && x.f" would still fail on nil.
			if (e->op == TK_ANDAND)
			{
				if (!Truthy(a))
				{
					Value r; r.kind = VK_BOOL; r.b = false; return r;
				}
				Value b = EvalExpr(in, e->items[1], env);
				Value r; r.kind = VK_BOOL; r.b = Truthy(b); return r;
			}
			if (e->op == TK_OROR)
			{
				if (Truthy(a))
				{
					Value r; r.kind = VK_BOOL; r.b = true; return r;
				}
				Value b = EvalExpr(in, e->items[1], env);
				Value r; r.kind = VK_BOOL; r.b = Truthy(b); return r;
			}

			{
				Value b = EvalExpr(in, e->items[1], env);
				Value r;

				switch (e->op)
				{
				case TK_EQ: r.kind = VK_BOOL; r.b = ValueEquals(a, b); return r;
				case TK_NE: r.kind = VK_BOOL; r.b = !ValueEquals(a, b); return r;
				case TK_LT: case TK_LE: case TK_GT: case TK_GE:
					if ((a.kind != VK_INT) || (b.kind != VK_INT))
					{
						throw Error(0x80070057, L"ordering compares integers only", e->line);
					}
					r.kind = VK_BOOL;
					if (e->op == TK_LT) r.b = a.i <  b.i;
					if (e->op == TK_LE) r.b = a.i <= b.i;
					if (e->op == TK_GT) r.b = a.i >  b.i;
					if (e->op == TK_GE) r.b = a.i >= b.i;
					return r;
				default:
					break;
				}

				// Arithmetic. Same types only: 1 + "1" is an error, not "11".
				if (e->op == TK_PLUS && (a.kind == VK_STR) && (b.kind == VK_STR))
				{
					r.kind = VK_STR;
					r.s = a.s + b.s;
					return r;
				}
				// Lists concatenate. The one place where + is not arithmetic, and
				// the reason "collect one handle per loop turn" needs no mutable
				// push: the operand is copied, so the result shares nothing with
				// either side.
				if ((e->op == TK_PLUS) && (a.kind == VK_LIST) && (b.kind == VK_LIST))
				{
					r.kind = VK_LIST;
					r.list = ListPtr(new std::vector<Value>());
					if (a.list)
					{
						r.list->insert(r.list->end(), a.list->begin(), a.list->end());
					}
					if (b.list)
					{
						r.list->insert(r.list->end(), b.list->begin(), b.list->end());
					}
					return r;
				}
				if ((a.kind != VK_INT) || (b.kind != VK_INT))
				{
					throw Error(0x80070057, L"arithmetic needs two integers (use ${...} to build a string)", e->line);
				}
				r.kind = VK_INT;
				switch (e->op)
				{
				// The arithmetic wraps, which is what the language says it
				// does. Signed overflow is undefined in C++, so the work is
				// done in the unsigned type and cast back: that is exactly
				// two's-complement wraparound, defined, instead of a value the
				// optimiser is allowed to invent.
				case TK_PLUS:  r.i = (long long) ((unsigned long long) a.i + (unsigned long long) b.i); break;
				case TK_MINUS: r.i = (long long) ((unsigned long long) a.i - (unsigned long long) b.i); break;
				case TK_STAR:  r.i = (long long) ((unsigned long long) a.i * (unsigned long long) b.i); break;
				case TK_SLASH:
					if (b.i == 0)
					{
						throw Error(0x80070057, L"division by zero", e->line);
					}
					// The one division that traps in hardware rather than
					// answering: the result is not representable. Wrapping, as
					// the operators above do, gives the most negative value.
					r.i = ((a.i == c_i64Min) && (b.i == -1)) ? c_i64Min : (a.i / b.i);
					break;
				case TK_PERCENT:
					if (b.i == 0)
					{
						throw Error(0x80070057, L"division by zero", e->line);
					}
					r.i = ((a.i == c_i64Min) && (b.i == -1)) ? 0 : (a.i % b.i);
					break;
				default:
					throw Error(0x80070057, L"unsupported operator", e->line);
				}
				return r;
			}
		}

		Value EvalExpr(Interp& in, const ExprPtr& e, const EnvPtr& env)
		{
			Value r;

			Tick(in, e->line);
			switch (e->kind)
			{
			case EX_NIL:
				return r;
			case EX_BOOL:
				r.kind = VK_BOOL; r.b = e->b; return r;
			case EX_INT:
				r.kind = VK_INT; r.i = e->i; return r;
			case EX_STR:
				r.kind = VK_STR; r.s = e->s; return r;
			case EX_INTERP:
				{
					String out;
					for (size_t i = 0; i < e->items.size(); i++)
					{
						out += ToDisplay(EvalExpr(in, e->items[i], env));
					}
					r.kind = VK_STR;
					r.s = out;
					return r;
				}
			case EX_LIST:
				{
					r.kind = VK_LIST;
					r.list = ListPtr(new std::vector<Value>());
					for (size_t i = 0; i < e->items.size(); i++)
					{
						r.list->push_back(CopyValue(EvalExpr(in, e->items[i], env)));
					}
					return r;
				}
			case EX_RECORD:
				{
					r.kind = VK_RECORD;
					r.rec = RecordPtr(new std::vector<std::pair<String, Value> >());
					for (size_t i = 0; i < e->keys.size(); i++)
					{
						r.rec->push_back(std::make_pair(e->keys[i], CopyValue(EvalExpr(in, e->items[i], env))));
					}
					return r;
				}
			case EX_VAR:
				{
					Value *found = LookupVar(env, e->name);
					if (!found)
					{
						throw Error(0x80070002, String(L"undefined name '") + e->name + L"'", e->line);
					}
					return *found;
				}
			case EX_INDEX:
				{
					Value base = EvalExpr(in, e->items[0], env);
					Value idx = EvalExpr(in, e->items[1], env);

					if (base.kind == VK_LIST)
					{
						long long i = AsInt(idx, e->line, L"a list index");
						if ((i < 0) || (i >= (long long) base.list->size()))
						{
							throw Error(0x80070057, L"list index out of range", e->line);
						}
						return (*base.list)[(size_t) i];
					}
					if (base.kind == VK_STR)
					{
						long long i = AsInt(idx, e->line, L"a string index");
						if ((i < 0) || (i >= (long long) base.s.size()))
						{
							throw Error(0x80070057, L"string index out of range", e->line);
						}
						r.kind = VK_STR;
						r.s = String(1, base.s[(size_t) i]);
						return r;
					}
					if (base.kind == VK_RECORD)
					{
						String key = AsStr(idx, e->line, L"a record key");
						for (size_t k = 0; k < base.rec->size(); k++)
						{
							if ((*base.rec)[k].first == key)
							{
								return (*base.rec)[k].second;
							}
						}
						throw Error(0x80070002, String(L"no such field '") + key + L"'", e->line);
					}
					throw Error(0x80070057, L"cannot index this value", e->line);
				}
			case EX_MEMBER:
				{
					Value base = EvalExpr(in, e->items[0], env);
					if (base.kind != VK_RECORD)
					{
						throw Error(0x80070057, String(L"'.': '") + e->name + L"' needs a record", e->line);
					}
					for (size_t k = 0; k < base.rec->size(); k++)
					{
						if ((*base.rec)[k].first == e->name)
						{
							return (*base.rec)[k].second;
						}
					}
					throw Error(0x80070002, String(L"no such field '") + e->name + L"'", e->line);
				}
			case EX_CALL:
				{
					std::vector<Value> args;
					for (size_t i = 0; i < e->items.size(); i++)
					{
						args.push_back(EvalExpr(in, e->items[i], env));
					}
					const HostEntry *host = FindHost(e->name);
					if (host)
					{
						return CallHost(in, e->name, args, e->line);
					}
					{
						Value *found = LookupVar(env, e->name);
						if (found && (found->kind == VK_FUNC))
						{
							return CallFunc(in, found->fn, args, e->line);
						}
					}
					throw Error(0x80070002, String(L"unknown function '") + e->name + L"'", e->line);
				}
			case EX_UNARY:
				{
					Value a = EvalExpr(in, e->items[0], env);
					if (e->op == TK_NOT)
					{
						r.kind = VK_BOOL; r.b = !Truthy(a); return r;
					}
					// Same wraparound rule as the arithmetic operators, so
					// negating the most negative value is defined too.
					r.kind = VK_INT;
					r.i = (long long) (0ULL - (unsigned long long) AsInt(a, e->line, L"unary '-'"));
					return r;
				}
			case EX_BINARY:
				return EvalBinary(in, e, env);
			case EX_FUNC:
				{
					FuncPtr fn(new FuncValue());
					fn->params = e->params;
					fn->body = e->fnBody;
					fn->env = env;
					r.kind = VK_FUNC;
					r.fn = fn;
					return r;
				}
			}
			return r;
		}

		void AssignTarget(Interp& in, const ExprPtr& target, const Value& value, const EnvPtr& env)
		{
			if (target->kind == EX_VAR)
			{
				Value *found = LookupVar(env, target->name);
				if (!found)
				{
					throw Error(0x80070002, String(L"assignment to undefined name '") + target->name + L"' (declare it with let)", target->line);
				}
				*found = CopyValue(value);
				return;
			}
			if (target->kind == EX_INDEX)
			{
				Value base = EvalExpr(in, target->items[0], env);
				Value idx = EvalExpr(in, target->items[1], env);
				if ((base.kind == VK_LIST) && base.list)
				{
					long long i = AsInt(idx, target->line, L"a list index");
					if ((i < 0) || (i >= (long long) base.list->size()))
					{
						throw Error(0x80070057, L"list index out of range", target->line);
					}
					(*base.list)[(size_t) i] = CopyValue(value);
					return;
				}
				throw Error(0x80070057, L"can only assign into a list element", target->line);
			}
			if (target->kind == EX_MEMBER)
			{
				throw Error(0x80070057, L"records are immutable", target->line);
			}
			throw Error(0x80070057, L"cannot assign to this expression", target->line);
		}

		void ExecBlock(Interp& in, const std::vector<StmtPtr>& body, const EnvPtr& env)
		{
			for (size_t i = 0; i < body.size(); i++)
			{
				ExecStmt(in, body[i], env);
			}
		}

		void ExecStmt(Interp& in, const StmtPtr& s, const EnvPtr& env)
		{
			Tick(in, s->line);
			switch (s->kind)
			{
			case ST_LET:
				env->vars[s->name] = CopyValue(EvalExpr(in, s->expr, env));
				return;
			case ST_ASSIGN:
				{
					Value v = EvalExpr(in, s->expr2, env);
					AssignTarget(in, s->expr, v, env);
					return;
				}
			case ST_EXPR:
				EvalExpr(in, s->expr, env);
				return;
			case ST_IF:
				if (Truthy(EvalExpr(in, s->expr, env)))
				{
					ExecBlock(in, s->body, EnvPtr(new Env(env)));
				}
				else
				{
					ExecBlock(in, s->elseBody, EnvPtr(new Env(env)));
				}
				return;
			case ST_FOR:
				{
					Value it = EvalExpr(in, s->expr, env);
					if (it.kind != VK_LIST)
					{
						throw Error(0x80070057, L"'for' needs a list", s->line);
					}
					for (size_t i = 0; i < it.list->size(); i++)
					{
						EnvPtr iter(new Env(env));
						iter->vars[s->name] = CopyValue((*it.list)[i]);
						try
						{
							ExecBlock(in, s->body, iter);
						}
						catch (BreakSignal&)
						{
							break;
						}
						catch (ContinueSignal&)
						{
							continue;
						}
					}
					return;
				}
			case ST_WHILE:
				while (Truthy(EvalExpr(in, s->expr, env)))
				{
					try
					{
						ExecBlock(in, s->body, EnvPtr(new Env(env)));
					}
					catch (BreakSignal&)
					{
						break;
					}
					catch (ContinueSignal&)
					{
						continue;
					}
				}
				return;
			case ST_RETURN:
				{
					ReturnSignal sig;
					sig.value = s->expr ? EvalExpr(in, s->expr, env) : Value();
					throw sig;
				}
			case ST_FUNC:
				{
					Value v;
					FuncPtr fn(new FuncValue());
					fn->params = s->params;
					fn->body = s->body;
					fn->env = env;
					v.kind = VK_FUNC;
					v.fn = fn;
					env->vars[s->name] = v;
					return;
				}
			case ST_TRY:
				try
				{
					ExecBlock(in, s->body, EnvPtr(new Env(env)));
				}
				catch (Error& err)
				{
					Value e;
					e.kind = VK_RECORD;
					e.rec = RecordPtr(new std::vector<std::pair<String, Value> >());
					{
						Value code; code.kind = VK_INT; code.i = err.code;
						Value msg;  msg.kind = VK_STR;  msg.s = err.message;
						Value site; site.kind = VK_INT; site.i = err.line;
						e.rec->push_back(std::make_pair(String(L"code"), code));
						e.rec->push_back(std::make_pair(String(L"message"), msg));
						e.rec->push_back(std::make_pair(String(L"line"), site));
					}
					EnvPtr handler(new Env(env));
					handler->vars[s->name] = e;
					ExecBlock(in, s->catchBody->body, handler);
				}
				return;
			}
		}
	}	// namespace

	void RunProgram(Interp& in, const std::vector<StmtPtr>& program)
	{
		ExecBlock(in, program, in.env);
	}
}
