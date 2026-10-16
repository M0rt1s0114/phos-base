//////////////////////////////////////////////////////////////////////////////////////////////////
// Phosphor - the structured script front end.
//
// This is the first executable slice of docs/LANGUAGE.md. It is a tree walking
// interpreter, deliberately small: it exists so the real HTBPE boot chain can
// be written in one language instead of PECMD dialect + cmd + Lua, not so that
// every design decision in the language document is finished.
//
// Two things are on purpose:
//   * values have value semantics, implemented with copy-on-write: a container
//     is shared when it is bound or passed, and cloned before it is written
//     through, so a write through one name is never visible through another
//     (SPEC-7)
//   * a function value captures the values its body uses and nothing else, so a
//     closure made in a loop does not keep that loop's frame alive (SPEC-4).
//     A top level function is not a closure: it is a module table entry, looked
//     up by name, which is how two of them can call each other
//   * a host call that fails stops the script unless it is written inside a
//     try block (decision 9)
//
// The host table at the bottom of host.cpp is the whole surface: every verb is a
// function that calls a capability directly. It used to be a bridge that rebuilt
// the text of the old command and handed it back to the old parser, which is also
// where the quoting bugs lived; that layer is gone, and so is the bridge.
//////////////////////////////////////////////////////////////////////////////////////////////////
#ifndef LANG_RUNTIME_H
#define LANG_RUNTIME_H

#include <map>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>

namespace Lang
{
	typedef std::wstring String;

	enum ValueKind { VK_NIL = 0, VK_BOOL, VK_INT, VK_STR, VK_LIST, VK_RECORD, VK_FUNC };

	struct Value;
	struct Env;
	struct FuncValue;
	struct Interp;

	typedef std::shared_ptr<std::vector<Value> > ListPtr;
	typedef std::shared_ptr<std::vector<std::pair<String, Value> > > RecordPtr;
	typedef std::shared_ptr<FuncValue> FuncPtr;

	struct Value
	{
		ValueKind kind;
		bool      b;
		long long i;
		String    s;
		ListPtr   list;
		RecordPtr rec;
		FuncPtr   fn;

		Value() : kind(VK_NIL), b(false), i(0) {}
	};

	enum TokenKind
	{
		TK_END = 0, TK_NEWLINE, TK_IDENT, TK_INT, TK_STR, TK_ESCSTR,
		TK_LPAREN, TK_RPAREN, TK_LBRACK, TK_RBRACK, TK_LBRACE, TK_RBRACE,
		TK_COMMA, TK_COLON, TK_DOT, TK_SEMI,
		TK_ASSIGN, TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE,
		TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
		TK_NOT, TK_ANDAND, TK_OROR
	};

	struct Token
	{
		TokenKind    kind;
		String       text;		// identifier, string payload, integer literal
		long long    i;
		int          line;
		Token() : kind(TK_END), i(0), line(0) {}
	};

	enum ExprKind
	{
		EX_NIL = 0, EX_BOOL, EX_INT, EX_STR, EX_INTERP, EX_LIST, EX_RECORD,
		EX_VAR, EX_INDEX, EX_MEMBER, EX_CALL, EX_BINARY, EX_UNARY, EX_FUNC
	};

	struct Expr;
	typedef std::shared_ptr<Expr> ExprPtr;
	struct Stmt;
	typedef std::shared_ptr<Stmt> StmtPtr;

	struct Expr
	{
		ExprKind             kind;
		int                  line;
		int                  op;			// binary/unary operator token kind
		bool                 b;
		long long            i;
		String               s;				// EX_STR payload
		String               name;			// variable / member / callee name
		std::vector<ExprPtr> items;			// list items, call args, operands, interp parts
		std::vector<String>  keys;			// record keys
		std::vector<String>  params;		// function parameters
		std::vector<int>     paramsLine;
		std::vector<StmtPtr> fnBody;		// EX_FUNC: the block body
		std::vector<String>  captures;		// EX_FUNC: free names, filled by the resolver
		Expr() : kind(EX_NIL), line(0), op(0), b(false), i(0) {}
	};

	enum StmtKind
	{
		ST_LET = 0, ST_ASSIGN, ST_EXPR, ST_IF, ST_FOR, ST_WHILE, ST_RETURN,
	ST_FUNC, ST_TRY, ST_BREAK, ST_CONTINUE
	};

	struct Stmt
	{
		StmtKind             kind;
		int                  line;
		String               name;			// let target / for variable / fn name / catch variable
		ExprPtr              expr;			// init / condition / iterable / return value / assign target
		ExprPtr              expr2;			// assign value
		std::vector<StmtPtr> body;
		std::vector<StmtPtr> elseBody;
		std::vector<String>  params;
		std::vector<int>     paramsLine;
		std::vector<String>  captures;		// ST_FUNC: free names, filled by the resolver
		StmtPtr              catchBody;
		Stmt() : kind(ST_EXPR), line(0) {}
	};

	// --- errors and non local exits -------------------------------------------------
	struct Error
	{
		long long    code;
		String       message;
		int          line;
		// The verb that was running when this was raised, when there was one.
		// Stamped on the way out of the host call rather than read when the
		// catch block runs: by then the call has returned and the interpreter is
		// no longer inside it. SPEC-6 puts this in the record a script sees.
		String       verb;
		Error(long long c, const String& m, int l) : code(c), message(m), line(l) {}
	};
	struct ReturnSignal { Value value; };
	struct BreakSignal {};
	struct ContinueSignal {};

	// A watchdog stop: the step budget. SPEC-6 says the budget cannot be taken
	// by `try/catch`, and the reason is mechanical -- the budget is spent for
	// the rest of the entry, so a handler would raise it again on its first
	// statement and the report would name the handler's line instead of the
	// one that ran away. The call-depth ceiling is deliberately NOT in here:
	// the stack unwinds, so a handler can recover from it, and case 025 does.
	struct GuardSignal
	{
		long long code;
		String    message;
		int       line;
		GuardSignal(long long c, const String& m, int l) : code(c), message(m), line(l) {}
	};

	struct Env
	{
		std::shared_ptr<Env> parent;
		std::map<String, Value> vars;
#ifdef PHOS_TESTHOOKS
		// How many Env objects are alive right now.
		//
		// A closure stored in its own scope makes an Env -> Value -> Env cycle
		// that no other check can see, because the interpreter never reports a
		// leak: the count simply never comes back down. Test builds only, and
		// the release binary must not carry it.
		static volatile LONG live;
#endif
		explicit Env(const std::shared_ptr<Env>& p) : parent(p)
		{
#ifdef PHOS_TESTHOOKS
			InterlockedIncrement(&live);
#endif
		}
#ifdef PHOS_TESTHOOKS
		~Env() { InterlockedDecrement(&live); }
#endif
	};

#ifdef PHOS_TESTHOOKS
	// How many deep copies of a list or a record have been made, so a test can
	// say that handing a large value around must not copy it every time. With
	// copy-on-write where a copy is not written to, these become reference-count
	// bumps and stop counting. Defined in host.cpp; test builds only.
	extern volatile LONG g_deepCopies;
#endif

	struct FuncValue
	{
		std::vector<String>  params;
		std::vector<StmtPtr> body;
		std::shared_ptr<Env> env;
	};

	typedef Value (*HostFn)(Interp&, std::vector<Value>&, int);

	// Is there a verb by this name? The resolver asks before letting a script
	// declare a function: a call looks in the verb table first, so a function
	// named after a verb would be declared and then never run.
	BOOL HostHasVerb(const String& name);

	// What a verb does to the world, declared beside the function so the two
	// cannot drift apart.
	//
	// `--dry-run` reads this: a verb whose grade is not SE_READ is refused and
	// traced instead of run. What it replaced was a blacklist of names kept
	// somewhere else, and an entry missing from a blacklist silently counts as
	// read only -- a verb added here without a grade does not compile, which is
	// the point.
	enum SideEffect
	{
		SE_READ = 0,	// observes; changes nothing that outlives the call
		SE_WRITE		// changes something, and the change is meant to last
	};

	struct HostEntry
	{
		const wchar_t *name;	// dotted verb, e.g. L"proc.run"
		HostFn         fn;
		SideEffect     side;
	};

	struct Interp
	{
		std::shared_ptr<Env> globals;
		std::shared_ptr<Env> env;
		UINT64               steps;
		UINT64               stepLimit;
		// Live call depth and its ceiling. Every call frame is a C++ frame with
		// a handful of Values on it, so a runaway recursion runs the real stack
		// out long before the step limit notices; the ceiling turns that into
		// an error the script can see.
		int                  depth;
		int                  depthLimit;
		BOOL                 dryRun;
		// The verb being executed, for the error record a catch block receives
		// (SPEC-6: code, message, line, verb) and for the crash breadcrumb. Set
		// around every host call, so it is the innermost one that failed.
		String               verb;
		String               scriptDir;		// directory of the running script, no trailing slash
		String               scriptPath;
		// A reference to itself, set where the interpreter is created.
		//
		// A closure bound to a hotkey has to run after the script that made it
		// has returned, and it needs an interpreter to run in -- this is how the
		// binding gets hold of one without the whole program having to know where
		// the interpreter lives.
		std::weak_ptr<Interp> self;
		// Process handles that proc.spawn handed out and nobody has awaited yet,
		// keyed by pid. The value a script sees is just { pid: N }, so the VM
		// stays free of native handles and the handles stay in one place.
		std::map<long long, HANDLE> procs;
		// Trail file per running task, keyed the same way. A task started with
		// run(path, { wait: false }) writes its own log, so two tasks cannot
		// interleave into one stream; the parent copies each one back into its
		// own channel when the task is awaited (docs/LANGUAGE.md ruling 21).
		std::map<long long, String> procLogs;

		Interp() : steps(0), stepLimit(20000000ULL), depth(0), depthLimit(200), dryRun(FALSE) {}
	};

	// --- the pieces -----------------------------------------------------------------
	BOOL IsMarker(const BYTE *pb, UINT uLen);
	std::vector<Token> Tokenize(const String& src, String& err);
	bool ParseProgram(const std::vector<Token>& toks, std::vector<StmtPtr>& out, String& err);

	String ToDisplay(const Value& v);
	String ToText(const Value& v);
	bool Truthy(const Value& v);
	bool ValueEquals(const Value& a, const Value& b, int line);

	const HostEntry *FindHost(const String& name);
	Value CallHost(Interp& in, const String& name, std::vector<Value>& args, int line);

	void RegisterBuiltins(Interp& in);
	void RunProgram(Interp& in, const std::vector<StmtPtr>& program);

	// Parse and run the bytes of a #!phos script. Returns S_OK, or a nonzero
	// HRESULT on an uncaught error (the error line is written to the trace).
	HRESULT RunBytes(const BYTE *pb, UINT uLen, PCTSTR ptzPath);

	// Parse only: fills errOut with a message and returns false when the script
	// has a syntax or name error. This is the host side check from the design
	// document, the one verification that needs no PE.
	bool CheckSource(const String& src, String& errOut);
	bool CheckFile(PCTSTR ptzPath, String& errOut);
}

#endif // LANG_RUNTIME_H
