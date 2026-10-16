//////////////////////////////////////////////////////////////////////////////////////////////////
// Phosphor interpreter.
//
// A tree walking evaluator over the parser's output. Values have value
// semantics implemented with copy-on-write: binding, arguments and capture
// share a container, and it is cloned only when something writes through it
// (SPEC-7). Only false and nil are falsy (section 2), and a failing host call
// raises unless the script wraps it in try (decision 9).
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
	const long long c_i64Max = 9223372036854775807LL;

	// How deep equality will follow a value before it stops.
	//
	// Data is not meant to nest this far, and a script that builds such a value
	// is a runaway, not a data structure. The ceiling turns that into a script
	// error naming the line instead of a stack the process never comes back
	// from, the way the call ceiling does for recursion. It is roomy: a real
	// boot config nests a handful of levels.
	const int c_valueDepthLimit = 1000;

	// A list or record is shared, not copied, when a value is bound to another
	// name, passed to a function or captured by a closure (SPEC-7). This is the
	// write side of that bargain: a container that more than one value refers
	// to is cloned before it is written through, so the other holders keep the
	// value they had.
	//
	// Only the container is cloned and its elements are shared until they too
	// are written through. That is what keeps the common shape -- hand a big
	// list to a reader -- free, which is the whole point of copy-on-write.
	void CloneIfShared(Value& v)
	{
		if ((v.kind == VK_LIST) && v.list && (v.list.use_count() > 1))
		{
#ifdef PHOS_TESTHOOKS
			InterlockedIncrement(&g_deepCopies);
#endif
			v.list = ListPtr(new std::vector<Value>(*v.list));
		}
		else if ((v.kind == VK_RECORD) && v.rec && (v.rec.use_count() > 1))
		{
#ifdef PHOS_TESTHOOKS
			InterlockedIncrement(&g_deepCopies);
#endif
			v.rec = RecordPtr(new std::vector<std::pair<String, Value> >(*v.rec));
		}
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

	// What "${...}" does with a value.
	//
	// nil, a list, a record and a function are refused here. Spelling them --
	// "nil", "(list)" -- produces a string that looks like an answer:
	// "${root}\\x" with root unset handed "nil\x" to fs.*, and the file
	// operation then failed somewhere else entirely. The rule for this language
	// is that a mistake stops the run, so this says what is wrong where it
	// happens.
	String ToInterpolated(const Value& v, int line)
	{
		switch (v.kind)
		{
		case VK_NIL:
			throw Error(0x80070057, L"cannot put nil into a string (is the name spelled right?)", line);
		case VK_LIST:
			throw Error(0x80070057, L"cannot put a list into a string", line);
		case VK_RECORD:
			throw Error(0x80070057, L"cannot put a record into a string", line);
		case VK_FUNC:
			throw Error(0x80070057, L"cannot put a function into a string", line);
		default:
			break;
		}
		return ToDisplay(v);
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

	// Structural equality, with a ceiling on how deep it follows a value.
	//
	// Two values are the same when they are the same kind and the same content:
	// two separately built lists are equal when their elements are, and a record
	// does not care what order its fields were written in. Functions compare by
	// identity -- two closures are the same only when they are the same object.
	//
	// The pointer check at the top of each container case is a fast path for
	// "the same value", not the rule: a shared value equals itself by content
	// anyway, and a value only ever shares with an equal one.
	bool ValueEqualsDepth(const Value& a, const Value& b, int depth, int line)
	{
		if (depth > c_valueDepthLimit)
		{
			throw Error(0x80070057, L"value nested past the depth limit (runaway data structure?)", line);
		}
		if (a.kind != b.kind)
		{
			return false;
		}
		switch (a.kind)
		{
		case VK_NIL:  return true;
		case VK_BOOL: return a.b == b.b;
		case VK_INT:  return a.i == b.i;
		case VK_STR:  return a.s == b.s;
		case VK_LIST:
			{
				if (a.list == b.list)
				{
					return true;
				}
				if (!a.list || !b.list || (a.list->size() != b.list->size()))
				{
					return false;
				}
				for (size_t i = 0; i < a.list->size(); i++)
				{
					if (!ValueEqualsDepth((*a.list)[i], (*b.list)[i], depth + 1, line))
					{
						return false;
					}
				}
				return true;
			}
		case VK_RECORD:
			{
				if (a.rec == b.rec)
				{
					return true;
				}
				if (!a.rec || !b.rec || (a.rec->size() != b.rec->size()))
				{
					return false;
				}
				// Field order is not part of a record's value, so each field of
				// one is looked up by name in the other.
				for (size_t i = 0; i < a.rec->size(); i++)
				{
					const String& key = (*a.rec)[i].first;
					bool found = false;
					for (size_t k = 0; k < b.rec->size(); k++)
					{
						if ((*b.rec)[k].first == key)
						{
							if (!ValueEqualsDepth((*a.rec)[i].second, (*b.rec)[k].second, depth + 1, line))
							{
								return false;
							}
							found = true;
							break;
						}
					}
					if (!found)
					{
						return false;
					}
				}
				return true;
			}
		case VK_FUNC: return a.fn == b.fn;
		}
		return false;
	}

	bool ValueEquals(const Value& a, const Value& b, int line)
	{
		return ValueEqualsDepth(a, b, 0, line);
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

		// The scope a closure carries: the values its body uses, copied out at
		// the moment it is created (SPEC-4, capture by value).
		//
		// Its parent is the module scope, not the scope the closure was written
		// in. That is what keeps a closure created inside a loop from holding the
		// loop frame alive: an Env -> Value -> Env cycle is invisible to
		// reference counting, so the frame would never be freed. A name that is
		// not captured still reaches the module scope by name, which is how a
		// closure calls a top level function.
		EnvPtr MakeClosureEnv(Interp& in, const EnvPtr& env, const std::vector<String>& names)
		{
			EnvPtr capture(new Env(in.env));
			for (size_t i = 0; i < names.size(); i++)
			{
				Value *found = LookupVar(env, names[i]);
				if (found)
				{
					capture->vars[names[i]] = *found;
				}
			}
			return capture;
		}

		void Tick(Interp& in, int line)
		{
			// The breadcrumb the crash handler reads. One store per statement,
			// which is what makes "line 37" mean anything in the record.
			CrashSetLine(line);
			if (++in.steps > in.stepLimit)
			{
				// Not an Error on purpose: SPEC-6 keeps the watchdog out of the
				// language's `catch`, which only takes Error.
				throw GuardSignal(0x8000FFFF, L"step limit reached (runaway loop?)", line);
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
				explicit DepthGuard(Interp& target) : in(target) { in.depth++; CrashSetDepth(in.depth); }
				~DepthGuard() { in.depth--; CrashSetDepth(in.depth); }
			} guard(in);

			// Arguments are bound into the callee. A container is shared, and a
			// write on either side clones it first, so a list passed in cannot
			// be written through from the other side (SPEC-7).
			for (size_t i = 0; i < fn->params.size(); i++)
			{
				call->vars[fn->params[i]] = args[i];
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
				case TK_EQ: r.kind = VK_BOOL; r.b = ValueEquals(a, b, e->line); return r;
				case TK_NE: r.kind = VK_BOOL; r.b = !ValueEquals(a, b, e->line); return r;
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

				// Arithmetic. Same types only, which is decision 3 in the language
				// document: 1 + "1" is an error, not "11".
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
				// These used to wrap, deliberately, and the language reference
				// said so. SPEC-8 changed the rule: a sum that cannot be
				// represented stops the script. Silently wrapping is the kind
				// of wrong answer this language is built to refuse, and it is
				// also the kind that survives to the end of a boot before
				// anybody notices.
				case TK_PLUS:
					if (((b.i > 0) && (a.i > c_i64Max - b.i)) ||
						((b.i < 0) && (a.i < c_i64Min - b.i)))
					{
						throw Error(0x80070057, L"integer overflow in '+'", e->line);
					}
					r.i = a.i + b.i;
					break;
				case TK_MINUS:
					if (((b.i < 0) && (a.i > c_i64Max + b.i)) ||
						((b.i > 0) && (a.i < c_i64Min + b.i)))
					{
						throw Error(0x80070057, L"integer overflow in '-'", e->line);
					}
					r.i = a.i - b.i;
					break;
				case TK_STAR:
					{
						// Magnitudes in the unsigned type, then one division to
						// see whether the product fits. Signed multiplication
						// that overflows is undefined in C++; this never
						// overflows and the answer is exact.
						unsigned long long ua = (a.i < 0)
							? (0ULL - (unsigned long long) a.i) : (unsigned long long) a.i;
						unsigned long long ub = (b.i < 0)
							? (0ULL - (unsigned long long) b.i) : (unsigned long long) b.i;
						unsigned long long up = ua * ub;

						if ((ub != 0) && ((up / ub) != ua))
						{
							throw Error(0x80070057, L"integer overflow in '*'", e->line);
						}
						// Room for the negative side is one larger than for the
						// positive one, and only there.
						if (up > ((a.i < 0) != (b.i < 0)
							? ((unsigned long long) c_i64Max + 1ULL)
							: (unsigned long long) c_i64Max))
						{
							throw Error(0x80070057, L"integer overflow in '*'", e->line);
						}
						r.i = (long long) (((a.i < 0) != (b.i < 0)) ? (0ULL - up) : up);
					}
					break;
				case TK_SLASH:
					if (b.i == 0)
					{
						throw Error(0x80070057, L"division by zero", e->line);
					}
					// The one division that traps in hardware rather than
					// answering: the result is not representable. It used to
					// answer with the most negative value, which is a made up
					// number; say so instead.
					if ((a.i == c_i64Min) && (b.i == -1))
					{
						throw Error(0x80070057, L"integer overflow in '/'", e->line);
					}
					r.i = a.i / b.i;
					break;
				case TK_PERCENT:
					if (b.i == 0)
					{
						throw Error(0x80070057, L"division by zero", e->line);
					}
					if ((a.i == c_i64Min) && (b.i == -1))
					{
						throw Error(0x80070057, L"integer overflow in '%'", e->line);
					}
					r.i = a.i % b.i;
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
						out += ToInterpolated(EvalExpr(in, e->items[i], env), e->line);
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
						r.list->push_back(EvalExpr(in, e->items[i], env));
					}
					return r;
				}
			case EX_RECORD:
				{
					r.kind = VK_RECORD;
					r.rec = RecordPtr(new std::vector<std::pair<String, Value> >());
					for (size_t i = 0; i < e->keys.size(); i++)
					{
						r.rec->push_back(std::make_pair(e->keys[i], EvalExpr(in, e->items[i], env)));
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
					r.kind = VK_INT;
					{
						long long iValue = AsInt(a, e->line, L"unary '-'");

						// The one value whose negation does not exist. It used
						// to come back as itself.
						if (iValue == c_i64Min)
						{
							throw Error(0x80070057, L"integer overflow in unary '-'", e->line);
						}
						r.i = -iValue;
					}
					return r;
				}
			case EX_BINARY:
				return EvalBinary(in, e, env);
			case EX_FUNC:
				{
					FuncPtr fn(new FuncValue());
					fn->params = e->params;
					fn->body = e->fnBody;
					fn->env = MakeClosureEnv(in, env, e->captures);
					r.kind = VK_FUNC;
					r.fn = fn;
					return r;
				}
			}
			return r;
		}

		// The Value an lvalue expression names, as a pointer into the
		// environment, or NULL when the expression is not something that can be
		// written in place.
		//
		// This exists because copy-on-write needs the write to land in the
		// variable's own slot. Reading the base with EvalExpr would make a copy
		// first, and writing to that copy would be thrown away -- the classic
		// bug where "a[i] = x" quietly changes a temporary. Along the way down
		// each shared container is cloned, so the write reaches one slot and the
		// other holders keep the value they had.
		Value *ResolveLValue(Interp& in, const ExprPtr& e, const EnvPtr& env)
		{
			if (e->kind == EX_VAR)
			{
				return LookupVar(env, e->name);
			}
			if (e->kind == EX_INDEX)
			{
				Value *base = ResolveLValue(in, e->items[0], env);
				if (!base)
				{
					return NULL;
				}
				CloneIfShared(*base);
				Value idx = EvalExpr(in, e->items[1], env);
				if ((base->kind == VK_LIST) && base->list)
				{
					long long i = AsInt(idx, e->line, L"a list index");
					if ((i < 0) || (i >= (long long) base->list->size()))
					{
						throw Error(0x80070057, L"list index out of range", e->line);
					}
					return &(*base->list)[(size_t) i];
				}
				if ((base->kind == VK_RECORD) && base->rec)
				{
					// A record is immutable, so its fields are not lvalues
					// either -- "a.b[0] = x" must not reach into the record and
					// write through a list it happens to hold.
					throw Error(0x80070057, L"records are immutable", e->line);
				}
				throw Error(0x80070057, L"can only assign into a list element", e->line);
			}
			return NULL;
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
				*found = value;
				return;
			}
			if (target->kind == EX_INDEX)
			{
				Value *slot = ResolveLValue(in, target, env);
				if (!slot)
				{
					throw Error(0x80070057, L"can only assign into a list element", target->line);
				}
				*slot = value;
				return;
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
				env->vars[s->name] = EvalExpr(in, s->expr, env);
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
						iter->vars[s->name] = (*it.list)[i];
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
			// Leaving a loop early. The signals are caught by the loop that owns
			// them (ST_FOR and ST_WHILE below); the resolver refuses one written
			// outside a loop, so nothing else can see them.
			case ST_BREAK:
				throw BreakSignal();
			case ST_CONTINUE:
				throw ContinueSignal();
			case ST_FUNC:
				{
					Value v;
					FuncPtr fn(new FuncValue());
					fn->params = s->params;
					fn->body = s->body;
					// A function declared at the top level is a module table
					// entry: it is looked up by name at call time, which is what
					// lets two functions written one after the other call each
					// other. Declared inside a body, it is a closure instead and
					// carries the values it uses.
					fn->env = (env == in.env)
						? env
						: MakeClosureEnv(in, env, s->captures);
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
						// SPEC-6 fixes the record at {code, message, line, verb}.
						//
						// *code* is an HRESULT. A verb that failed reports the
						// raw Win32 code, which is a small positive number, so
						// it is wrapped here -- at the boundary -- rather than at
						// every throw site. A script comparing against an
						// HRESULT then sees one, and `E_FAIL` and `2` stop being
						// the same kind of thing.
						long long iCode = (long long) err.code;
						Value code;
						Value msg;
						Value site;
						Value verb;

						if ((iCode > 0) && (iCode <= 0xFFFF))
						{
							iCode = (long long) HRESULT_FROM_WIN32((DWORD) iCode);
						}
						code.kind = VK_INT; code.i = iCode;
						msg.kind = VK_STR;  msg.s = err.message;
						site.kind = VK_INT; site.i = err.line;
						verb.kind = VK_STR;
						// The verb the error was raised in, stamped on the way out
						// of the host call; in.verb is the fallback for an error
						// raised somewhere else while a verb was still running.
						verb.s = !err.verb.empty()
							? err.verb
							: (in.verb.empty() ? String(L"(none)") : in.verb);
						e.rec->push_back(std::make_pair(String(L"code"), code));
						e.rec->push_back(std::make_pair(String(L"message"), msg));
						e.rec->push_back(std::make_pair(String(L"line"), site));
						e.rec->push_back(std::make_pair(String(L"verb"), verb));
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
