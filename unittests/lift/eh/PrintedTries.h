//===- PrintedTries.h - The __try nesting of printed C -----------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What tests of printed SEH ask of a function's C text: which `__try`
/// statements hold a place in it, and whether every `goto` lands where C
/// lets it.  C forbids a jump into a `__try`.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_EH_PRINTEDTRIES_H
#define NEVERD_UNITTESTS_LIFT_EH_PRINTEDTRIES_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace neverd::printed_tries {

/// Walks \p Function line by line and calls \p AtLine with each line's offset
/// and the tries open where it begins, by the number of their `__try`.
/// Returns false when the braces do not balance.
template <typename Callback>
bool forEachLine(llvm::StringRef Function, Callback AtLine) {
  struct Brace {
    bool IsTry = false;
    unsigned Try = 0;
  };
  std::vector<Brace> Open;
  unsigned Tries = 0;
  size_t Offset = 0;
  while (Offset <= Function.size()) {
    const size_t End = std::min(Function.find('\n', Offset), Function.size());
    const llvm::StringRef Line = Function.slice(Offset, End);
    std::vector<unsigned> Held;
    for (const Brace &B : Open)
      if (B.IsTry)
        Held.push_back(B.Try);
    AtLine(Offset, Line, Held);
    const bool OpensTry = Line.trim() == "__try {";
    for (char C : Line) {
      if (C == '}') {
        if (Open.empty())
          return false;
        Open.pop_back();
      } else if (C == '{') {
        Open.push_back({OpensTry, OpensTry ? ++Tries : 0});
      }
    }
    Offset = End + 1;
  }
  return Open.empty();
}

/// The tries that hold the line of \p Position in \p Function.
inline std::vector<unsigned> at(llvm::StringRef Function, size_t Position) {
  std::vector<unsigned> Found;
  forEachLine(Function, [&](size_t Offset, llvm::StringRef Line,
                            const std::vector<unsigned> &Held) {
    if (Position >= Offset && Position <= Offset + Line.size())
      Found = Held;
  });
  return Found;
}

/// How many tries hold the first line of \p Function with \p Needle, or -1.
inline int depthOf(llvm::StringRef Function, llvm::StringRef Needle) {
  const size_t Position = Function.find(Needle);
  if (Position == llvm::StringRef::npos)
    return -1;
  // A needle that begins with the end of the line before it names the next.
  const size_t Skipped = Needle.find_first_not_of('\n');
  return static_cast<int>(
      at(Function, Position + (Skipped == llvm::StringRef::npos ? 0 : Skipped))
          .size());
}

/// Empty, or what is wrong with the jumps of \p Function: the first `goto`
/// whose label stands inside a `__try` that does not hold the `goto`.
inline std::string jumpIntoATry(llvm::StringRef Function) {
  std::map<std::string, std::vector<unsigned>> Labels;
  std::vector<std::pair<std::string, std::vector<unsigned>>> Jumps;
  const bool Balanced =
      forEachLine(Function, [&](size_t, llvm::StringRef Line,
                                const std::vector<unsigned> &Held) {
        llvm::StringRef Text = Line.trim();
        if (!Line.empty() && Line.front() != ' ' && Text.ends_with(":") &&
            Text.find(' ') == llvm::StringRef::npos)
          Labels[Text.drop_back().str()] = Held;
        else if (Text.consume_front("goto "))
          Jumps.emplace_back(
              Text.take_until([](char C) { return C == ';'; }).str(), Held);
      });
  if (!Balanced)
    return "the braces do not balance";
  for (const auto &[Label, From] : Jumps) {
    const auto To = Labels.find(Label);
    if (To == Labels.end())
      return "goto " + Label + " names no label";
    if (To->second.size() > From.size() ||
        !std::equal(To->second.begin(), To->second.end(), From.begin()))
      return "goto " + Label + " enters a __try";
  }
  return {};
}

} // namespace neverd::printed_tries

#endif // NEVERD_UNITTESTS_LIFT_EH_PRINTEDTRIES_H
