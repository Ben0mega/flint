// Copyright (c) 2014- Facebook
// License: Boost License 1.0, http://boost.org/LICENSE_1_0.txt

#include "Tokenizer.h"
#include "folly/Range.h"

#include <map>

namespace facebook { namespace flint {

using namespace std;
using namespace folly;

static map<StringPiece, TokenType> initializeKeywords() {
  map<StringPiece, TokenType> result;
#define CPPLINT_ASSIGN(s, tk) result[StringPiece(s, strlen(s))] = tk;
  CPPLINT_FORALL_KEYWORDS(CPPLINT_ASSIGN)
#undef CPPLINT_ASSIGN
  return result;
}

#define FBEXCEPTION(...) \
  do { throw runtime_error(to<string>(__VA_ARGS__)); } while (0)

#define ENFORCE(e, ...) \
  if (e) {} else FBEXCEPTION(__VA_ARGS__);

/**
 * Map containing mappings of the kind "virtual" -> TK_VIRTUAL.
 */
auto static keywords = initializeKeywords();

/**
 * Eats howMany characters out of pc, avances pc appropriately, and
 * returns the eaten portion.
 */
static StringPiece munchChars(StringPiece & pc, size_t howMany) {
  assert(pc.size() >= howMany);
  auto result = pc.subpiece(0, howMany);
  pc.advance(howMany);
  return result;
}

/**
 * Assuming pc is positioned at the start of an identifier, munches it
 * from pc and returns it.
 */
static StringPiece munchIdentifier(StringPiece & pc) {
  for (size_t i = 0; ; ++i) {
    assert(i < pc.size());
    auto const c = pc[i];
    // g++ allows '$' in identifiers. Also, some crazy inline
    // assembler uses '@' in identifiers, see e.g.
    // fbcode/external/cryptopp/rijndael.cpp, line 527
    if (!isalnum(c) && c != '_' && c != '$' && c != '@') {
      // done
      ENFORCE(i > 0, "Invalid identifier: ", pc);
      return munchChars(pc, i);
    }
  }
  assert(false);
}

/**
 * Assuming pc is positioned at the start of a C-style comment,
 * munches it from pc and returns it.
 */
static StringPiece munchComment(StringPiece & pc, size_t & line) {
  assert(pc[0] == '/' && pc[1] == '*');
  for (size_t i = 2; ; ++i) {
    assert(i < pc.size());
    auto c = pc[i];
    if (c == '\n') {
      ++line;
    }
    else if (c == '*') {
      if (pc[i + 1] == '/') {
        // end of comment
        return munchChars(pc, i + 2);
      }
    }
    else if (!c) {
      // end of input
      FBEXCEPTION("Unterminated comment: ", pc);
    }
  }
  assert(false);
}

/**
 * Assuming pc is positioned at the start of a single-line comment,
 * munches it from pc and returns it.
 */
static StringPiece munchSingleLineComment(StringPiece & pc, size_t & line) {
  assert(pc[0] == '/' && pc[1] == '/');
  for (size_t i = 2; ; ++i) {
    assert(i < pc.size());
    auto c = pc[i];
    if (c == '\n') {
      ++line;
      if (i > 0 && pc[i - 1] == '\\') {
        // multiline single-line comment (sic)
        continue;
      }
      // end of comment
      return munchChars(pc, i + 1);
    }
    if (!c) {
      // single-line comment at end of file, meh
      return munchChars(pc, i);
    }
  }
  assert(false);
}

/**
 * Assuming pc is positioned at the start of a number (be it decimal
 * or floating-point), munches it off pc and returns it. Note that the
 * number is assumed to be correct so a number of checks are not
 * necessary.
 */
static StringPiece munchNumber(StringPiece & pc) {
  bool sawDot = false, sawExp = false, sawX = false, sawSuffix = false;
  for (size_t i = 0; ; ++i) {
    assert(i < pc.size());
    auto const c = pc[i];
    if (c == '.' && !sawDot && !sawExp && !sawSuffix) {
      sawDot = true;
    } else if (isdigit(c)) {
      // Nothing to do
    } else if (sawX && !sawExp && c && strchr("AaBbCcDdEeFf", c)) {
      // Hex digit; nothing to do. The condition includes !sawExp
      // because the exponent is decimal even in a hex floating-point
      // number!
    } else if (c == '+' || c == '-') {
      // Sign may appear at the start or right after E or P
      if (i > 0 && !strchr("EePp", pc[i - 1])) {
        // Done, the sign is the next token
        return munchChars(pc, i);
      }
    } else if (!sawExp && !sawSuffix && !sawX && (c == 'e' || c == 'E')) {
      sawExp = true;
    } else if (sawX && !sawExp && !sawSuffix && (c == 'p' || c == 'P')) {
      sawExp = true;
    } else if ((c == 'x' || c == 'X') && i == 1 && pc[0] == '0') {
      sawX = true;
    } else if (c && strchr("FfLlUu", c)) {
      // It's a suffix. There could be several of them (including
      // repeats a la LL), so let's not return just yet
      sawSuffix = true;
    } else {
      // done
      ENFORCE(i > 0, "Invalid number: ", pc);
      return munchChars(pc, i);
    }
  }
  assert(false);
}

/**
 * Assuming pc is positioned at the start of a character literal,
 * munches it from pc and returns it. A reference to line is passed in
 * order to track multiline character literals (yeah, that can
 * actually happen) correctly.
 */
static StringPiece munchCharLiteral(StringPiece & pc, size_t & line) {
  assert(pc[0] == '\'');
  for (size_t i = 1; ; ++i) {
    auto const c = pc[i];
    if (c == '\'') {
      // That's about it
      return munchChars(pc, i + 1);
    }
    if (c == '\\') {
      ++i;
      if (pc[i] == '\n') {
        ++line;
      }
      continue;
    }
    ENFORCE(c, "Unterminated character constant: ", pc);
  }
}

/**
 * Assuming pc is positioned at the start of a string literal, munches
 * it from pc and returns it. A reference to line is passed in order
 * to track multiline strings correctly.
 */
static StringPiece munchString(StringPiece & pc, size_t & line) {
  assert(pc[0] == '"');
  for (size_t i = 1; ; ++i) {
    auto const c = pc[i];
    if (c == '"') {
      // That's about it
      return munchChars(pc, i + 1);
    }
    if (c == '\\') {
      ++i;
      if (pc[i] == '\n') {
        ++line;
      }
      continue;
    }
    ENFORCE(c, "Unterminated string constant: ", pc);
  }
}

/**
 * Munches horizontal spaces from pc. If we want to disallow tabs in
 * sources, here is the place. No need for end=of-input checks as the
 * input always has a '\0' at the end.
 */
StringPiece munchSpaces(StringPiece & pc) {
  size_t i = 0;
  for (; pc[i] == ' ' || pc[i] == '\t'; ++i) {
  }
  auto const result = StringPiece(pc.data(), i);
  pc.advance(i);
  return result;
}

/**
 * Given the contents of a C++ file and a filename, tokenizes the
 * contents and places it in output.
 */
void tokenize(const string & input, const string & initialFilename,
              vector<Token> & output) {
  output.resize(0);
  // The string piece includes the terminating nul character
  StringPiece pc(input.c_str(), input.size() + 1);
  StringPiece file = initialFilename;
  size_t line = 1;

  TokenType t;
  size_t tokenLen;
  //StringPiece comment;
  size_t preTokenLen = 0;
  const char * preTokenPtr = pc.data();

  while (1) {
    auto const c = pc.front();

    switch (c) {
      // *** One-character tokens that don't require lookahead (comma,
      // *** semicolon etc.)
#define CPPLINT_INTRODUCE_CASE(c0, t0)                          \
      case (c0): t = (t0); tokenLen = 1; goto INSERT_TOKEN;
      CPPLINT_FORALL_ONE_CHAR_TOKENS(CPPLINT_INTRODUCE_CASE);
#undef CPPLINT_INTRODUCE_CASE
     // *** One- or two-character tokens
#define CPPLINT_INTRODUCE_CASE(c1, t1, c2, t2)                          \
      case c1:                                                          \
        if (pc[1] == (c2)) {                                            \
          t = (t2); tokenLen = 2;                                       \
        } else {                                                        \
          t = (t1); tokenLen = 1;                                       \
        }                                                               \
        goto INSERT_TOKEN;
      // Insert a bunch of code here
      CPPLINT_FORALL_ONE_OR_TWO_CHAR_TOKENS(CPPLINT_INTRODUCE_CASE);
#undef CPPLINT_INTRODUCE_CASE
#define CPPLINT_INTRODUCE_CASE(c1, t1, c2, t2, c3, t3)                  \
      case c1:                                                          \
        if (pc[1] == (c2)) {                                            \
          t = (t2); tokenLen = 2;                                       \
        } else if (pc[1] == (c3)) {                                     \
          t = (t3); tokenLen = 2;                                       \
        } else {                                                        \
          t = (t1); tokenLen = 1;                                       \
        }                                                               \
        goto INSERT_TOKEN;
      // Insert a bunch of code here
      CPPLINT_FORALL_ONE_OR_TWO_CHAR_TOKENS2(CPPLINT_INTRODUCE_CASE);
#undef CPPLINT_INTRODUCE_CASE
#define CPPLINT_INTRODUCE_CASE(c1, t1, c2, t2, c3, t3, c4, t4)          \
          case c1:                                                      \
            if (pc[1] == (c2)) {                                        \
              t = (t2); tokenLen = 2;                                   \
            } else if (pc[1] == (c3)) {                                 \
              if (pc[2] == (c4)) {                                      \
                t = (t4); tokenLen = 3;                                 \
              } else {                                                  \
                t = (t3); tokenLen = 2;                                 \
              }                                                         \
            } else {                                                    \
              t = (t1); tokenLen = 1;                                   \
            }                                                           \
            goto INSERT_TOKEN;
            // Insert a bunch of code here
            CPPLINT_FORALL_ONE_TO_THREE_CHAR_TOKENS(CPPLINT_INTRODUCE_CASE);
#undef CPPLINT_INTRODUCE_CASE
      // *** Everything starting with a slash: /, /=, single- and
      // *** multi-line comments
      case '/':
        if (pc[1] == '*') {
          //comment = munchComment(pc, line);
          preTokenLen += munchComment(pc, line).size();
          break;
        }
        if (pc[1] == '/') {
          //comment = munchSingleLineComment(pc, line);
          preTokenLen += munchSingleLineComment(pc, line).size();
          break;
        }
        if (pc[1] == '=') {
          t = TK_DIVIDE_ASSIGN; tokenLen = 2;
        } else {
          t = TK_DIVIDE; tokenLen = 1;
        }
        goto INSERT_TOKEN;
        // *** Backslash
      case '\\':
        ENFORCE(pc[1] == '\n' || pc[1] == '\r',
                "Misplaced backslash in ", file, ":", line);
        ++line;
        pc.advance(2);
        preTokenLen += 2;
        break;
      // *** Newline
      case '\n':
        pc.advance(1);
        ++preTokenLen;
        ++line;
        break;
      // *** Part of a DOS newline; ignored
      case '\r':
        pc.advance(1);
        ++preTokenLen;
        break;
      // *** ->, --, -=, ->*, and -
      case '-':
        if (pc[1] == '-') {
          t = TK_DECREMENT; tokenLen = 2;
          goto INSERT_TOKEN;
        }
        if (pc[1] == '=') {
          t = TK_MINUS_ASSIGN; tokenLen = 2;
          goto INSERT_TOKEN;
        }
        if (pc[1] == '>') {
          if (pc[2] == '*') {
            t = TK_ARROW_STAR; tokenLen = 3;
          } else {
            t = TK_ARROW; tokenLen = 2;
          }
          goto INSERT_TOKEN;
        }
        t = TK_MINUS; tokenLen = 1;
        goto INSERT_TOKEN;
      // *** Whitespace
      case ' ': case '\t':
        preTokenLen += munchSpaces(pc).size();
        break;
      // *** Done parsing!
      case '\0':
        assert(pc.size() == 1);
        // Push last token, the EOF
        output.push_back(Token(TK_EOF, pc, file, line,
                               preTokenPtr, preTokenLen));
        return;
      // *** Verboten characters (do allow '@' and '$' as extensions)
      case '`':
        FBEXCEPTION("Invalid character: ", c, " in ", file, ":", line);
        break;
      // *** Numbers
      case '0': case '1': case '2': case '3': case '4': case '5':
      case '6': case '7': case '8': case '9':
      ITS_A_NUMBER: {
          auto symbol = munchNumber(pc);
          assert(symbol.size() > 0);
          output.push_back(Token(TK_NUMBER, symbol, file, line,
                                 preTokenPtr, preTokenLen));
        }
        break;
      // *** Number, member selector, ellipsis, or .*
      case '.':
        if (isdigit(pc[1])) {
          goto ITS_A_NUMBER;
        }
        if (pc[1] == '*') {
          t = TK_DOT_STAR; tokenLen = 2;
        } else if (pc[1] == '.' && pc[2] == '.') {
          t = TK_ELLIPSIS; tokenLen = 3;
        } else {
          t = TK_DOT; tokenLen = 1;
        }
        goto INSERT_TOKEN;
      // *** Character literal
      case '\'':
      {
        auto charLit = munchCharLiteral(pc, line);
        output.push_back(Token(TK_CHAR_LITERAL, charLit, file, line,
                               preTokenPtr, preTokenLen));
      }
      break;
      // *** String literal
      case '"':
      {
        auto str = munchString(pc, line);
        output.push_back(Token(TK_STRING_LITERAL, str, file, line,
                               preTokenPtr, preTokenLen));
      }
      break;
      case '#': {
        // Skip ws
        auto pc1 = pc;
        pc1.advance(1);
        tokenLen = 1 + munchSpaces(pc1).size();
        // define, include, pragma, or line
        if (pc1.startsWith("line")) {
          t = TK_HASHLINE; tokenLen += qfind(pc1, '\n');
        } else if (pc1.startsWith("warning") || pc1.startsWith("error")) {
          // The entire #warning/#error line is the token value
          t = TK_ERROR; tokenLen += qfind(pc1, '\n');
          ENFORCE(tokenLen > 0, "Unterminated #warning/#error message");
        } else if (pc1.startsWith("include")) {
          t = TK_INCLUDE; tokenLen += strlen("include");
        } else if (pc1.startsWith("ifdef")) {
          t = TK_IFDEF; tokenLen += strlen("ifdef");
        } else if (pc1.startsWith("ifndef")) {
          t = TK_IFNDEF; tokenLen += strlen("ifndef");
        } else if (pc1.startsWith("if")) {
          t = TK_POUNDIF; tokenLen += strlen("if");
        } else if (pc1.startsWith("undef")) {
          t = TK_UNDEF; tokenLen += strlen("undef");
        } else if (pc1.startsWith("else")) {
          t = TK_POUNDELSE; tokenLen += strlen("else");
        } else if (pc1.startsWith("endif")) {
          t = TK_ENDIF; tokenLen += strlen("endif");
        } else if (pc1.startsWith("define")) {
          t = TK_DEFINE; tokenLen += strlen("define");
        } else if (pc1.startsWith("pragma")) {
          t = TK_PRAGMA; tokenLen += strlen("pragma");
        } else if (pc1.startsWith("#")) {
          t = TK_DOUBLEPOUND; tokenLen += strlen("##");
        } else {
          // We can only assume this is inside a macro definition
          t = TK_POUND; tokenLen += 1;
        }
      }
        goto INSERT_TOKEN;
      // *** Everything else
      default:
        if (iscntrl(c)) {
          pc.advance(1);
          ++preTokenLen;
        } else if (isalpha(c) || c == '_' || c == '$' || c == '@') {
          // it's a word
          auto symbol = munchIdentifier(pc);
          auto iter = keywords.find(symbol);
          if (iter != keywords.end()) {
            // keyword, baby
            output.push_back(Token(iter->second, symbol, file, line,
                                   preTokenPtr, preTokenLen));
          } else {
            // Some identifier
            assert(symbol.size() > 0);
            output.push_back(Token(TK_IDENTIFIER, symbol, file, line,
                                   preTokenPtr, preTokenLen));
          }
        } else {
          // what could this be? (BOM?)
          FBEXCEPTION("Unrecognized character in ",
                      initialFilename, ":", line);
        }
        break;
        // *** All
      INSERT_TOKEN:
        output.push_back(Token(t, munchChars(pc, tokenLen), file, line,
                               preTokenPtr, preTokenLen));
        preTokenPtr = pc.data();
        preTokenLen = 0;
        break;
    }
  }
  assert(false);
}

/**
 * Converts e.g. TK_VIRTUAL to "TK_VIRTUAL".
 */
string toString(TokenType t) {
#define CPPLINT_X1(c1, t1) if ((t1) == t) return (#t1);
#define CPPLINT_X2(c1, t1, c2, t2) CPPLINT_X1(c1, t1) CPPLINT_X1(c2, t2)
#define CPPLINT_X3(c1, t1, c2, t2, c3, t3)      \
  CPPLINT_X1(c1, t1) CPPLINT_X2(c2, t2, c3, t3)
#define CPPLINT_X4(c1, t1, c2, t2, c3, t3, c4, t4)               \
  CPPLINT_X2(c1, t1, c2, t2) CPPLINT_X2(c3, t3, c4, t4)
// Expansion
  CPPLINT_FOR_ALL_TOKENS(CPPLINT_X1, CPPLINT_X2, CPPLINT_X3, CPPLINT_X4)
#undef CPPLINT_X1
#undef CPPLINT_X2
#undef CPPLINT_X3
#undef CPPLINT_X4
    FBEXCEPTION("Unknown token type: ", t);
}

} /*namespace flint*/ } /*namespace facebook*/
