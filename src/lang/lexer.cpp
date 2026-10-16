//////////////////////////////////////////////////////////////////////////////////////////////////
// Phosphor lexer.
//
// String literals do not process escapes, which is decision 16 in
// docs/LANGUAGE.md: "X:\Windows\System32" is what it looks like. The rare case
// that needs a newline or a real quote writes esc"...". Both forms still take
// ${expr} interpolation, and the interpolation text is kept verbatim here and
// taken apart by the parser, because the expression inside it is a full one.
//////////////////////////////////////////////////////////////////////////////////////////////////
namespace Lang
{
	static bool IsIdentStart(wchar_t c)
	{
		return ((c >= L'a') && (c <= L'z')) || ((c >= L'A') && (c <= L'Z')) || (c == L'_');
	}

	static bool IsIdentPart(wchar_t c)
	{
		return IsIdentStart(c) || ((c >= L'0') && (c <= L'9'));
	}

	static bool IsDigit(wchar_t c)
	{
		return (c >= L'0') && (c <= L'9');
	}

	// Copy a ${ ... } run verbatim, honouring nested braces and any strings
	// inside. Returns the index just past the closing brace, or src.size() when
	// the run is unterminated (the parser reports the unbalanced source).
	static size_t CopyInterpolation(const String& src, size_t at, String& out)
	{
		int depth = 0;
		size_t i = at;

		while (i < src.size())
		{
			wchar_t c = src[i];

			if ((c == L'"') || ((c == L'e') && (src.compare(i, 4, L"esc\"") == 0)))
			{
				size_t q = ((c == L'e') ? i + 3 : i);
				out += src.substr(i, q - i + 1);
				i = q + 1;
				while ((i < src.size()) && (src[i] != L'"'))
				{
					if (src[i] == L'\\')
					{
						out += src[i++];
					}
					if (i < src.size())
					{
						out += src[i++];
					}
				}
				if (i < src.size())
				{
					out += src[i++];
				}
				continue;
			}
			if (c == L'{')
			{
				depth++;
			}
			else if (c == L'}')
			{
				depth--;
			}
			out += c;
			i++;
			if (depth == 0)
			{
				return i;
			}
		}
		return i;
	}

	// Scan the body of a string. `escaped` turns \n \t \r \\ \" into their
	// characters; both modes keep ${...} runs.
	static String ScanString(const String& src, size_t& i, bool escaped, bool& terminated)
	{
		String out;

		terminated = false;
		i++;			// past the opening quote
		while (i < src.size())
		{
			wchar_t c = src[i];

			if (c == L'"')
			{
				i++;
				terminated = true;
				return out;
			}
			if ((c == L'$') && (i + 1 < src.size()) && (src[i + 1] == L'{'))
			{
				i = CopyInterpolation(src, i, out);
				continue;
			}
			if (escaped && (c == L'\\') && (i + 1 < src.size()))
			{
				wchar_t n = src[i + 1];
				i += 2;
				switch (n)
				{
				case L'n': out += L'\n'; break;
				case L't': out += L'\t'; break;
				case L'r': out += L'\r'; break;
				case L'0': out += L'\0'; break;
				case L'\\': out += L'\\'; break;
				case L'"': out += L'"'; break;
				default: out += n; break;
				}
				continue;
			}
			out += c;
			i++;
		}
		return out;
	}

	std::vector<Token> Tokenize(const String& src, String& err)
	{
		std::vector<Token> toks;
		size_t i = 0;
		int line = 1;

		err.clear();
		while (i < src.size())
		{
			wchar_t c = src[i];

			if ((c == L' ') || (c == L'\t') || (c == 0xFEFF))
			{
				i++;
				continue;
			}
			if ((c == L'\r') || (c == L'\n'))
			{
				Token t;
				t.kind = TK_NEWLINE;
				t.line = line;
				toks.push_back(t);
				if ((c == L'\r') && (i + 1 < src.size()) && (src[i + 1] == L'\n'))
				{
					i++;
				}
				i++;
				line++;
				continue;
			}
			if ((c == L'/') && (i + 1 < src.size()) && (src[i + 1] == L'/'))
			{
				while ((i < src.size()) && (src[i] != L'\n') && (src[i] != L'\r'))
				{
					i++;
				}
				continue;
			}
			if ((c == L'/') && (i + 1 < src.size()) && (src[i + 1] == L'*'))
			{
				bool sawNewline = false;

				i += 2;
				while ((i + 1 < src.size()) && !((src[i] == L'*') && (src[i + 1] == L'/')))
				{
					if ((src[i] == L'\n') || (src[i] == L'\r'))
					{
						sawNewline = true;
						if ((src[i] == L'\r') && (src[i + 1] == L'\n'))
						{
							i++;
						}
						line++;
					}
					i++;
				}
				i = (i + 1 < src.size()) ? i + 2 : src.size();
				if (sawNewline)
				{
					Token t;
					t.kind = TK_NEWLINE;
					t.line = line;
					toks.push_back(t);
				}
				continue;
			}
			if ((c == L';'))
			{
				Token t;
				t.kind = TK_SEMI;
				t.line = line;
				toks.push_back(t);
				i++;
				continue;
			}
			if ((c == L'e') && (src.compare(i, 4, L"esc\"") == 0))
			{
				bool terminated = false;
				String body;
				Token t;

				i += 3;			// leave i on the quote
				body = ScanString(src, i, true, terminated);
				if (!terminated)
				{
					TCHAR tzBuf[128];
					UStrPrint(tzBuf, TEXT("line %d: unterminated string literal"), line);
					err = tzBuf;
					return toks;
				}
				t.kind = TK_ESCSTR;
				t.text = body;
				t.line = line;
				toks.push_back(t);
				continue;
			}
			if (c == L'"')
			{
				bool terminated = false;
				String body = ScanString(src, i, false, terminated);
				Token t;

				if (!terminated)
				{
					TCHAR tzBuf[128];
					UStrPrint(tzBuf, TEXT("line %d: unterminated string literal"), line);
					err = tzBuf;
					return toks;
				}
				t.kind = TK_STR;
				t.text = body;
				t.line = line;
				toks.push_back(t);
				continue;
			}
			if (IsDigit(c))
			{
				Token t;
				String digits;
				unsigned long long value = 0;
				bool bOverflow = false;

				t.kind = TK_INT;
				t.line = line;
				if ((c == L'0') && (i + 1 < src.size()) && ((src[i + 1] == L'x') || (src[i + 1] == L'X')))
				{
					i += 2;
					while ((i < src.size()) &&
						(IsDigit(src[i]) || ((src[i] >= L'a') && (src[i] <= L'f')) || ((src[i] >= L'A') && (src[i] <= L'F'))))
					{
						wchar_t d = src[i];
						unsigned digit = (d <= L'9') ? (unsigned) (d - L'0') : (unsigned) ((d | 0x20) - L'a' + 10);

						if (value > (0x7FFFFFFFFFFFFFFFULL - digit) / 16)
						{
							bOverflow = true;
						}
						value = value * 16 + digit;
						digits += d;
						i++;
					}
				}
				else
				{
					while ((i < src.size()) && IsDigit(src[i]))
					{
						unsigned digit = (unsigned) (src[i] - L'0');

						if (value > (0x7FFFFFFFFFFFFFFFULL - digit) / 10)
						{
							bOverflow = true;
						}
						value = value * 10 + digit;
						digits += src[i];
						i++;
					}
				}
				// A literal that does not fit is refused instead of wrapping:
				// the value would otherwise be wrong with nothing to show it.
				if (bOverflow)
				{
					TCHAR tzBuf[256];
					UStrPrint(tzBuf, TEXT("line %d: integer literal is out of range: %s"),
						line, digits.c_str());
					err = tzBuf;
					return toks;
				}
				t.i = (long long) value;
				t.text = digits;
				toks.push_back(t);
				continue;
			}
			if (IsIdentStart(c))
			{
				Token t;
				String name;

				while ((i < src.size()) && IsIdentPart(src[i]))
				{
					name += src[i++];
				}
				t.kind = TK_IDENT;
				t.text = name;
				t.line = line;
				toks.push_back(t);
				continue;
			}

			// Punctuation and operators. Two character forms come first.
			{
				Token t;
				wchar_t n = ((i + 1 < src.size()) ? src[i + 1] : 0);

				t.line = line;
				if ((c == L'=') && (n == L'=')) { t.kind = TK_EQ; i += 2; }
				else if ((c == L'!') && (n == L'=')) { t.kind = TK_NE; i += 2; }
				else if ((c == L'<') && (n == L'=')) { t.kind = TK_LE; i += 2; }
				else if ((c == L'>') && (n == L'=')) { t.kind = TK_GE; i += 2; }
				else if ((c == L'&') && (n == L'&')) { t.kind = TK_ANDAND; i += 2; }
				else if ((c == L'|') && (n == L'|')) { t.kind = TK_OROR; i += 2; }
				else if (c == L'=') { t.kind = TK_ASSIGN; i++; }
				else if (c == L'<') { t.kind = TK_LT; i++; }
				else if (c == L'>') { t.kind = TK_GT; i++; }
				else if (c == L'(') { t.kind = TK_LPAREN; i++; }
				else if (c == L')') { t.kind = TK_RPAREN; i++; }
				else if (c == L'[') { t.kind = TK_LBRACK; i++; }
				else if (c == L']') { t.kind = TK_RBRACK; i++; }
				else if (c == L'{') { t.kind = TK_LBRACE; i++; }
				else if (c == L'}') { t.kind = TK_RBRACE; i++; }
				else if (c == L',') { t.kind = TK_COMMA; i++; }
				else if (c == L':') { t.kind = TK_COLON; i++; }
				else if (c == L'.') { t.kind = TK_DOT; i++; }
				else if (c == L'+') { t.kind = TK_PLUS; i++; }
				else if (c == L'-') { t.kind = TK_MINUS; i++; }
				else if (c == L'*') { t.kind = TK_STAR; i++; }
				else if (c == L'/') { t.kind = TK_SLASH; i++; }
				else if (c == L'%') { t.kind = TK_PERCENT; i++; }
				else if (c == L'!') { t.kind = TK_NOT; i++; }
				else
				{
					TCHAR tzMsg[64];
					UStrPrint(tzMsg, TEXT("unexpected character U+%04X"), (UINT) c);
					err = tzMsg;
					return toks;
				}
				toks.push_back(t);
			}
		}

		{
			Token t;
			t.kind = TK_END;
			t.line = line;
			toks.push_back(t);
		}
		return toks;
	}
}
