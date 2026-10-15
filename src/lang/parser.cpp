//////////////////////////////////////////////////////////////////////////////////////////////////
// Phosphor parser: tokens in, statement tree out.
//
// Newlines separate statements, `;` is accepted and ignored, and an expression
// is allowed to run across a line break only after an operator, a comma or an
// opening bracket - which is what lets the tables in the startup scripts stay
// readable without making "a\nb" mean one call.
//////////////////////////////////////////////////////////////////////////////////////////////////
namespace Lang
{
	namespace
	{
		struct Parser
		{
			const std::vector<Token>& t;
			size_t                    p;
			String                    err;
			int                       depth;

			explicit Parser(const std::vector<Token>& toks) : t(toks), p(0), depth(0) {}

			const Token& Cur() const { return t[p < t.size() ? p : t.size() - 1]; }
			const Token& Peek(size_t n = 1) const { return t[(p + n) < t.size() ? (p + n) : (t.size() - 1)]; }
			bool At(TokenKind k) const { return Cur().kind == k; }
			bool AtIdent(const wchar_t *name) const { return (Cur().kind == TK_IDENT) && (Cur().text == name); }
			void Next() { if (p + 1 < t.size()) p++; }
			void SkipNL() { while (At(TK_NEWLINE)) Next(); }
			void SkipSeparators() { while (At(TK_NEWLINE) || At(TK_SEMI)) Next(); }

			bool Fail(const String& m)
			{
				if (err.empty())
				{
					TCHAR tzBuf[512];
					UStrPrint(tzBuf, TEXT("line %d: %s"), Cur().line, m.c_str());
					err = tzBuf;
				}
				return false;
			}

			bool Expect(TokenKind k, const wchar_t *what)
			{
				if (!At(k))
				{
					return Fail(String(L"expected ") + what);
				}
				Next();
				return true;
			}

			// --- statements ---------------------------------------------------------

			bool EndOfStatement()
			{
				if (At(TK_NEWLINE) || At(TK_SEMI))
				{
					SkipSeparators();
					return true;
				}
				if (At(TK_END) || At(TK_RBRACE))
				{
					return true;
				}
				return Fail(L"expected end of statement");
			}

			bool ParseBlock(std::vector<StmtPtr>& body)
			{
				SkipNL();
				if (!Expect(TK_LBRACE, L"'{'"))
				{
					return false;
				}
				SkipSeparators();
				while (!At(TK_RBRACE))
				{
					StmtPtr s;
					if (At(TK_END))
					{
						return Fail(L"unclosed block");
					}
					if (!ParseStmt(s))
					{
						return false;
					}
					body.push_back(s);
					if (!EndOfStatement())
					{
						return false;
					}
				}
				Next();			// the closing brace
				return true;
			}

			bool ParseParamList(std::vector<String>& params, std::vector<int>& lines)
			{
				SkipNL();
				if (!Expect(TK_LPAREN, L"'('"))
				{
					return false;
				}
				SkipNL();
				if (At(TK_RPAREN))
				{
					Next();
					return true;
				}
				while (true)
				{
					if (!At(TK_IDENT))
					{
						return Fail(L"expected parameter name");
					}
					params.push_back(Cur().text);
					lines.push_back(Cur().line);
					Next();
					SkipNL();
					if (At(TK_COMMA))
					{
						Next();
						SkipNL();
						if (At(TK_RPAREN))		// a trailing comma is allowed
						{
							break;
						}
						continue;
					}
					break;
				}
				return Expect(TK_RPAREN, L"')'");
			}

			bool ParseStmt(StmtPtr& out)
			{
				StmtPtr s(new Stmt());

				if (depth > 200)
				{
					return Fail(L"too deeply nested");
				}
				depth++;

				s->line = Cur().line;
				if (AtIdent(L"let"))
				{
					Next();
					if (!At(TK_IDENT))
					{
						return Fail(L"expected a name after 'let'");
					}
					s->kind = ST_LET;
					s->name = Cur().text;
					Next();
					if (!Expect(TK_ASSIGN, L"'='"))
					{
						return false;
					}
					if (!ParseExpr(s->expr))
					{
						return false;
					}
				}
				else if (AtIdent(L"if"))
				{
					Next();
					s->kind = ST_IF;
					if (!ParseExpr(s->expr))
					{
						return false;
					}
					if (!ParseBlock(s->body))
					{
						return false;
					}
					// Look past the line break for an else, but put it back when
					// there is none: that newline still has to terminate this
					// statement, or the next line reads as a continuation.
					{
						size_t save = p;
						SkipNL();
						if (!AtIdent(L"else"))
						{
							p = save;
						}
					}
					if (AtIdent(L"else"))
					{
						Next();
						SkipNL();
						if (AtIdent(L"if"))
						{
							StmtPtr nested;
							if (!ParseStmt(nested))
							{
								return false;
							}
							s->elseBody.push_back(nested);
						}
						else if (!ParseBlock(s->elseBody))
						{
							return false;
						}
					}
				}
				else if (AtIdent(L"while"))
				{
					Next();
					s->kind = ST_WHILE;
					if (!ParseExpr(s->expr))
					{
						return false;
					}
					if (!ParseBlock(s->body))
					{
						return false;
					}
				}
				else if (AtIdent(L"for"))
				{
					Next();
					if (!At(TK_IDENT))
					{
						return Fail(L"expected a loop variable after 'for'");
					}
					s->kind = ST_FOR;
					s->name = Cur().text;
					Next();
					if (!AtIdent(L"in"))
					{
						return Fail(L"expected 'in'");
					}
					Next();
					if (!ParseExpr(s->expr))
					{
						return false;
					}
					if (!ParseBlock(s->body))
					{
						return false;
					}
				}
				else if (AtIdent(L"return"))
				{
					Next();
					s->kind = ST_RETURN;
					if (!At(TK_NEWLINE) && !At(TK_SEMI) && !At(TK_RBRACE) && !At(TK_END))
					{
						if (!ParseExpr(s->expr))
						{
							return false;
						}
					}
				}
				else if (AtIdent(L"try"))
				{
					Next();
					s->kind = ST_TRY;
					if (!ParseBlock(s->body))
					{
						return false;
					}
					SkipNL();
					if (!AtIdent(L"catch"))
					{
						return Fail(L"expected 'catch' after the try block");
					}
					Next();
					if (!At(TK_IDENT))
					{
						return Fail(L"expected a name after 'catch'");
					}
					s->name = Cur().text;
					Next();
					s->catchBody = StmtPtr(new Stmt());
					if (!ParseBlock(s->catchBody->body))
					{
						return false;
					}
				}
				else if (AtIdent(L"fn") && (Peek().kind == TK_IDENT))
				{
					Next();
					s->kind = ST_FUNC;
					s->name = Cur().text;
					Next();
					if (!ParseParamList(s->params, s->paramsLine))
					{
						return false;
					}
					if (!ParseBlock(s->body))
					{
						return false;
					}
				}
				else
				{
					// An assignment or a bare expression. The two share the left
					// hand side, so parse one expression and look for '=' after it.
					ExprPtr lhs;
					if (!ParseExpr(lhs))
					{
						return false;
					}
					if (At(TK_ASSIGN))
					{
						Next();
						s->kind = ST_ASSIGN;
						s->expr = lhs;
						if (!ParseExpr(s->expr2))
						{
							return false;
						}
					}
					else
					{
						s->kind = ST_EXPR;
						s->expr = lhs;
					}
				}

				depth--;
				out = s;
				return true;
			}

			// --- expressions --------------------------------------------------------

			bool ParseExpr(ExprPtr& out)
			{
				SkipNL();
				return ParseOr(out);
			}

			bool MakeBinary(int op, const ExprPtr& lhs, const ExprPtr& rhs, ExprPtr& out)
			{
				ExprPtr e(new Expr());
				e->kind = EX_BINARY;
				e->op = op;
				e->line = lhs->line;
				e->items.push_back(lhs);
				e->items.push_back(rhs);
				out = e;
				return true;
			}

			bool ParseOr(ExprPtr& out)
			{
				if (!ParseAnd(out))
				{
					return false;
				}
				while (At(TK_OROR))
				{
					int op = Cur().kind;
					ExprPtr rhs;
					Next();
					SkipNL();
					if (!ParseAnd(rhs))
					{
						return false;
					}
					if (!MakeBinary(op, out, rhs, out))
					{
						return false;
					}
				}
				return true;
			}

			bool ParseAnd(ExprPtr& out)
			{
				if (!ParseEquality(out))
				{
					return false;
				}
				while (At(TK_ANDAND))
				{
					int op = Cur().kind;
					ExprPtr rhs;
					Next();
					SkipNL();
					if (!ParseEquality(rhs))
					{
						return false;
					}
					if (!MakeBinary(op, out, rhs, out))
					{
						return false;
					}
				}
				return true;
			}

			bool ParseEquality(ExprPtr& out)
			{
				if (!ParseComparison(out))
				{
					return false;
				}
				while (At(TK_EQ) || At(TK_NE))
				{
					int op = Cur().kind;
					ExprPtr rhs;
					Next();
					SkipNL();
					if (!ParseComparison(rhs))
					{
						return false;
					}
					if (!MakeBinary(op, out, rhs, out))
					{
						return false;
					}
				}
				return true;
			}

			bool ParseComparison(ExprPtr& out)
			{
				if (!ParseTerm(out))
				{
					return false;
				}
				while (At(TK_LT) || At(TK_LE) || At(TK_GT) || At(TK_GE))
				{
					int op = Cur().kind;
					ExprPtr rhs;
					Next();
					SkipNL();
					if (!ParseTerm(rhs))
					{
						return false;
					}
					if (!MakeBinary(op, out, rhs, out))
					{
						return false;
					}
				}
				return true;
			}

			bool ParseTerm(ExprPtr& out)
			{
				if (!ParseFactor(out))
				{
					return false;
				}
				while (At(TK_PLUS) || At(TK_MINUS))
				{
					int op = Cur().kind;
					ExprPtr rhs;
					Next();
					SkipNL();
					if (!ParseFactor(rhs))
					{
						return false;
					}
					if (!MakeBinary(op, out, rhs, out))
					{
						return false;
					}
				}
				return true;
			}

			bool ParseFactor(ExprPtr& out)
			{
				if (!ParseUnary(out))
				{
					return false;
				}
				while (At(TK_STAR) || At(TK_SLASH) || At(TK_PERCENT))
				{
					int op = Cur().kind;
					ExprPtr rhs;
					Next();
					SkipNL();
					if (!ParseUnary(rhs))
					{
						return false;
					}
					if (!MakeBinary(op, out, rhs, out))
					{
						return false;
					}
				}
				return true;
			}

			bool ParseUnary(ExprPtr& out)
			{
				if (At(TK_NOT) || At(TK_MINUS))
				{
					ExprPtr e(new Expr());
					ExprPtr operand;
					e->kind = EX_UNARY;
					e->op = Cur().kind;
					e->line = Cur().line;
					Next();
					SkipNL();
					if (!ParseUnary(operand))
					{
						return false;
					}
					e->items.push_back(operand);
					out = e;
					return true;
				}
				return ParsePostfix(out);
			}

			bool ParseArgs(std::vector<ExprPtr>& args)
			{
				SkipNL();
				if (!Expect(TK_LPAREN, L"'('"))
				{
					return false;
				}
				SkipNL();
				if (At(TK_RPAREN))
				{
					Next();
					return true;
				}
				while (true)
				{
					ExprPtr a;
					if (!ParseExpr(a))
					{
						return false;
					}
					args.push_back(a);
					SkipNL();
					if (At(TK_COMMA))
					{
						Next();
						SkipNL();
						continue;
					}
					break;
				}
				return Expect(TK_RPAREN, L"')'");
			}

			bool ParsePostfix(ExprPtr& out)
			{
				if (!ParsePrimary(out))
				{
					return false;
				}
				while (true)
				{
					if (At(TK_LPAREN))
					{
						ExprPtr e(new Expr());
						String callee;

						e->kind = EX_CALL;
						e->line = out->line;
						if (out->kind == EX_VAR)
						{
							callee = out->name;
						}
						else if ((out->kind == EX_MEMBER) && (out->items[0]->kind == EX_VAR))
						{
							// Host verbs are dotted (fs.list, proc.run, log.open), so a
							// member call on a plain name is a name, not a method.
							callee = out->items[0]->name + L"." + out->name;
						}
						else
						{
							return Fail(L"only a name can be called");
						}
						e->name = callee;
						if (!ParseArgs(e->items))
						{
							return false;
						}
						out = e;
						continue;
					}
					if (At(TK_LBRACK))
					{
						ExprPtr e(new Expr());
						ExprPtr index;
						e->kind = EX_INDEX;
						e->line = out->line;
						Next();
						SkipNL();
						if (!ParseExpr(index))
						{
							return false;
						}
						SkipNL();
						if (!Expect(TK_RBRACK, L"']'"))
						{
							return false;
						}
						e->items.push_back(out);
						e->items.push_back(index);
						out = e;
						continue;
					}
					if (At(TK_DOT))
					{
						ExprPtr e(new Expr());
						e->kind = EX_MEMBER;
						e->line = out->line;
						Next();
						if (!At(TK_IDENT))
						{
							return Fail(L"expected a name after '.'");
						}
						e->name = Cur().text;
						Next();
						e->items.push_back(out);
						out = e;
						continue;
					}
					break;
				}
				return true;
			}

			bool BuildString(const String& raw, int line, ExprPtr& out)
			{
				std::vector<ExprPtr> parts;
				size_t i = 0;
				size_t lit = 0;

				while (i < raw.size())
				{
					if ((raw[i] == L'$') && (i + 1 < raw.size()) && (raw[i + 1] == L'{'))
					{
						if (i > lit)
						{
							ExprPtr s(new Expr());
							s->kind = EX_STR;
							s->line = line;
							s->s = raw.substr(lit, i - lit);
							parts.push_back(s);
						}
						{
							size_t j = i + 2;
							int nNest = 1;
							size_t start = j;
							while ((j < raw.size()) && (nNest > 0))
							{
								if (raw[j] == L'{')
								{
									nNest++;
								}
								else if (raw[j] == L'}')
								{
									nNest--;
									if (nNest == 0)
									{
										break;
									}
								}
								j++;
							}
							if (j >= raw.size())
							{
								Fail(L"unterminated ${...} in a string");
								return false;
							}
							{
								String sub = raw.substr(start, j - start);
								String subErr;
								std::vector<Token> subToks = Tokenize(sub, subErr);
								Parser subParser(subToks);
								ExprPtr inner;

								if (!subErr.empty() || !subParser.ParseExpr(inner) ||
									!subParser.At(TK_END))
								{
									Fail(L"bad expression inside ${...}");
									return false;
								}
								inner->line = line;
								parts.push_back(inner);
							}
							i = j + 1;
							lit = i;
							continue;
						}
					}
					i++;
				}
				if (lit < raw.size())
				{
					ExprPtr s(new Expr());
					s->kind = EX_STR;
					s->line = line;
					s->s = raw.substr(lit);
					parts.push_back(s);
				}

				if (parts.size() == 1)
				{
					out = parts[0];
					return true;
				}
				{
					ExprPtr e(new Expr());
					e->kind = EX_INTERP;
					e->line = line;
					e->items = parts;
					out = e;
				}
				return true;
			}

			bool ParsePrimary(ExprPtr& out)
			{
				ExprPtr e(new Expr());

				e->line = Cur().line;
				if (At(TK_INT))
				{
					e->kind = EX_INT;
					e->i = Cur().i;
					Next();
					out = e;
					return true;
				}
				if (At(TK_STR) || At(TK_ESCSTR))
				{
					if (!BuildString(Cur().text, Cur().line, out))
					{
						return false;
					}
					Next();
					return true;
				}
				if (At(TK_IDENT))
				{
					if (Cur().text == L"nil") { e->kind = EX_NIL; Next(); out = e; return true; }
					if (Cur().text == L"true") { e->kind = EX_BOOL; e->b = true; Next(); out = e; return true; }
					if (Cur().text == L"false") { e->kind = EX_BOOL; e->b = false; Next(); out = e; return true; }
					if (Cur().text == L"fn")
					{
						Next();
						e->kind = EX_FUNC;
						if (!ParseParamList(e->params, e->paramsLine))
						{
							return false;
						}
						if (!ParseBlock(e->fnBody))
						{
							return false;
						}
						out = e;
						return true;
					}
					e->kind = EX_VAR;
					e->name = Cur().text;
					Next();
					out = e;
					return true;
				}
				if (At(TK_LPAREN))
				{
					Next();
					SkipNL();
					if (!ParseExpr(out))
					{
						return false;
					}
					SkipNL();
					return Expect(TK_RPAREN, L"')'");
				}
				if (At(TK_LBRACK))
				{
					Next();
					SkipNL();
					e->kind = EX_LIST;
					if (At(TK_RBRACK))
					{
						Next();
						out = e;
						return true;
					}
					while (true)
					{
						ExprPtr item;
						if (!ParseExpr(item))
						{
							return false;
						}
						e->items.push_back(item);
						SkipNL();
						if (At(TK_COMMA))
						{
							Next();
							SkipNL();
							if (At(TK_RBRACK))	// a trailing comma is allowed
							{
								break;
							}
							continue;
						}
						break;
					}
					if (!Expect(TK_RBRACK, L"']'"))
					{
						return false;
					}
					out = e;
					return true;
				}
				if (At(TK_LBRACE))
				{
					Next();
					SkipNL();
					e->kind = EX_RECORD;
					if (At(TK_RBRACE))
					{
						Next();
						out = e;
						return true;
					}
					while (true)
					{
						String key;
						ExprPtr value;

						if (At(TK_IDENT) || At(TK_STR) || At(TK_ESCSTR))
						{
							key = Cur().text;
							Next();
						}
						else
						{
							return Fail(L"expected a field name");
						}
						SkipNL();
						if (!Expect(TK_COLON, L"':'"))
						{
							return false;
						}
						SkipNL();
						if (!ParseExpr(value))
						{
							return false;
						}
						e->keys.push_back(key);
						e->items.push_back(value);
						SkipNL();
						if (At(TK_COMMA))
						{
							Next();
							SkipNL();
							if (At(TK_RBRACE))	// a trailing comma is allowed
							{
								break;
							}
							continue;
						}
						break;
					}
					if (!Expect(TK_RBRACE, L"'}'"))
					{
						return false;
					}
					out = e;
					return true;
				}
				return Fail(L"expected an expression");
			}

			bool ParseTop(std::vector<StmtPtr>& out)
			{
				SkipSeparators();
				while (!At(TK_END))
				{
					StmtPtr s;
					if (!ParseStmt(s))
					{
						return false;
					}
					out.push_back(s);
					if (!EndOfStatement())
					{
						return false;
					}
				}
				return true;
			}
		};
	}	// namespace

	bool ParseProgram(const std::vector<Token>& toks, std::vector<StmtPtr>& out, String& err)
	{
		Parser parser(toks);
		bool ok = parser.ParseTop(out);
		if (!ok)
		{
			err = parser.err;
		}
		return ok;
	}
}
