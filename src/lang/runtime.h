//////////////////////////////////////////////////////////////////////////////////////////////////
// Phosphor - the structured script front end.
//
// A tree-walking interpreter, deliberately small: it exists so that a PE boot
// chain can be written in one language instead of a command file plus cmd.
//
// Two things are on purpose:
//   * values are copied on assignment, so there is no aliasing and no cycles
//   * a host call that fails stops the script unless it is written inside a
//     try block
//
// The host table at the bottom of host.cpp is a bridge: every verb turns into
// the corresponding text command, so both forms of a script reach the same
// implementation.
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
		Expr() : kind(EX_NIL), line(0), op(0), b(false), i(0) {}
	};

	enum StmtKind
	{
		ST_LET = 0, ST_ASSIGN, ST_EXPR, ST_IF, ST_FOR, ST_WHILE, ST_RETURN,
		ST_FUNC, ST_TRY
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
		StmtPtr              catchBody;
		Stmt() : kind(ST_EXPR), line(0) {}
	};

	// --- errors and non local exits -------------------------------------------------
	struct Error
	{
		long long    code;
		String       message;
		int          line;
		Error(long long c, const String& m, int l) : code(c), message(m), line(l) {}
	};
	struct ReturnSignal { Value value; };
	struct BreakSignal {};
	struct ContinueSignal {};

	struct Env
	{
		std::shared_ptr<Env> parent;
		std::map<String, Value> vars;
		explicit Env(const std::shared_ptr<Env>& p) : parent(p) {}
	};

	struct FuncValue
	{
		std::vector<String>  params;
		std::vector<StmtPtr> body;
		std::shared_ptr<Env> env;
	};

	typedef Value (*HostFn)(Interp&, std::vector<Value>&, int);

	struct HostEntry
	{
		const wchar_t *name;	// dotted verb, e.g. L"proc.run"
		HostFn         fn;
	};

	struct Interp
	{
		std::shared_ptr<Env> globals;
		std::shared_ptr<Env> env;
		UINT64               steps;
		UINT64               stepLimit;
		BOOL                 dryRun;
		String               scriptDir;		// directory of the running script, no trailing slash
		String               scriptPath;
		// Process handles that proc.spawn handed out and nobody has awaited yet,
		// keyed by pid. The value a script sees is just { pid: N }, so the VM
		// stays free of native handles and the handles stay in one place.
		std::map<long long, HANDLE> procs;
		// Trail file per running task, keyed the same way. A task started with
		// run(path, { wait: false }) writes its own log, so two tasks cannot
		// interleave into one stream; the parent copies each one back into its
		// own channel when the task is awaited.
		std::map<long long, String> procLogs;

		Interp() : steps(0), stepLimit(20000000ULL), dryRun(FALSE) {}
	};

	// --- the pieces -----------------------------------------------------------------
	BOOL IsMarker(const BYTE *pb, UINT uLen);
	std::vector<Token> Tokenize(const String& src, String& err);
	bool ParseProgram(const std::vector<Token>& toks, std::vector<StmtPtr>& out, String& err);

	Value CopyValue(const Value& v);
	String ToDisplay(const Value& v);
	String ToText(const Value& v);
	bool Truthy(const Value& v);
	bool ValueEquals(const Value& a, const Value& b);

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
