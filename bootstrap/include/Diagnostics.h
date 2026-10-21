#pragma once
// Compiler-style error reports for the strict front-ends (C/C++ under
// --strict, Java), laid out the way the real compilers print them so that
// output from `qrun --strict` reads like gcc / javac output.

#include <string>
#include <vector>

namespace diag
{

// The 1-based `line` of `source` without its line terminator, or "".
std::string sourceLine(const std::string &source, int line);

// 1-based column of the first whole-word occurrence of `word` in `text`
// (an identifier not embedded in a longer one, outside string literals),
// or 0 when there is none.
int findWord(const std::string &text, const std::string &word);

// The first 'single-quoted' name in a message ("unknown name 'y'" -> "y").
std::string quotedName(const std::string &msg);

// gcc:
//   main.cpp:4:5: error: expected ';' before 'std'
//       4 |     std::cout << x;
//         |     ^~~
// `col` 0 = unknown (no caret line); `width` = characters to underline.
std::string gcc(const std::string &file, const std::string &source, int line, int col,
                const std::string &msg, int width = 1, const std::string &severity = "error");

// javac:
//   Main.java:5: error: ';' expected
//           int x = 5
//                    ^
// `notes` are extra indented lines javac prints below the caret
// ("  symbol:   variable y", "  location: class Main").
std::string javac(const std::string &file, const std::string &source, int line, int col,
                  const std::string &msg, const std::vector<std::string> &notes = {});

} // namespace diag
