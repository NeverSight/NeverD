//===- COFFARMPipelineTestsDetail.h - Windows ARM pipeline test harness -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Readers over lifter and decompiler output, plus the COFFARMPipeline
// fixture, shared by the pipeline translation units.
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_FORMAT_COFFARMPIPELINETESTSDETAIL_H
#define NEVERD_UNITTESTS_LIFT_FORMAT_COFFARMPIPELINETESTSDETAIL_H

#include "COFFARMFormatTestsDetail.h"

namespace neverd::coff_arm_test {

inline std::optional<std::string> lowFunctionBody(llvm::StringRef Output,
                                           llvm::StringRef Name) {
  std::string Header = (llvm::Twine("func ") + Name + " @").str();
  size_t Begin = Output.find(Header);
  if (Begin == llvm::StringRef::npos)
    return std::nullopt;
  size_t End = Output.find("\nfunc ", Begin + Header.size());
  return Output.slice(Begin, End == llvm::StringRef::npos ? Output.size() : End)
      .str();
}

inline std::optional<std::string> cFunctionBody(llvm::StringRef Output,
                                         llvm::StringRef Name) {
  std::string Header = (llvm::Twine(Name) + "(").str();
  size_t SearchFrom = 0;
  while (true) {
    size_t Begin = Output.find(Header, SearchFrom);
    if (Begin == llvm::StringRef::npos)
      return std::nullopt;
    size_t OpenBrace = Output.find('{', Begin + Header.size());
    size_t Semicolon = Output.find(';', Begin + Header.size());
    if (OpenBrace != llvm::StringRef::npos &&
        (Semicolon == llvm::StringRef::npos || OpenBrace < Semicolon)) {
      unsigned Depth = 0;
      for (size_t I = OpenBrace; I < Output.size(); ++I) {
        if (Output[I] == '{')
          ++Depth;
        else if (Output[I] == '}' && --Depth == 0)
          return Output.slice(Begin, I + 1).str();
      }
      return std::nullopt;
    }
    SearchFrom = Begin + Header.size();
  }
}

inline void expectLeafSemantics(llvm::StringRef Body) {
  EXPECT_TRUE(Body.contains("LOAD"));
  EXPECT_TRUE(Body.contains("STORE"));
  EXPECT_TRUE(Body.contains("INT_ADD"));
  EXPECT_TRUE(Body.contains("RETURN"));
  EXPECT_TRUE(Body.contains("INT_MULT") ||
              (Body.contains("INT_LEFT") && Body.contains("INT_ADD")))
      << "expected multiply semantics in pe_leaf:\n"
      << Body.str();
}

inline void expectStackySemantics(llvm::StringRef Body) {
  for (llvm::StringRef Opcode : {"LOAD", "STORE", "INT_ADD", "CALL",
                                 "RETURN"})
    EXPECT_TRUE(Body.contains(Opcode))
        << "expected " << Opcode.str() << " in pe_stacky:\n" << Body.str();
}

inline void expectNoOddFunctionAddresses(llvm::StringRef Output) {
  while (!Output.empty()) {
    auto [Line, Rest] = Output.split('\n');
    Output = Rest;
    if (!Line.starts_with("func "))
      continue;
    size_t Marker = Line.find(" @ 0x");
    ASSERT_NE(Marker, llvm::StringRef::npos) << Line.str();
    llvm::StringRef Hex = Line.drop_front(Marker + 5);
    size_t HexEnd = Hex.find_first_of(" \t(");
    Hex = Hex.take_front(HexEnd);
    uint64_t Addr = 0;
    ASSERT_FALSE(Hex.getAsInteger(16, Addr)) << Line.str();
    EXPECT_EQ(Addr & 1u, 0u) << "odd-address function: " << Line.str();
  }
}

inline void expectLeafCallResultStored(llvm::StringRef Body) {
  size_t Call = Body.find("pe_leaf(");
  ASSERT_NE(Call, llvm::StringRef::npos) << Body.str();
  size_t StatementStart = Body.rfind(';', Call);
  size_t OpenBrace = Body.rfind('{', Call);
  if (StatementStart == llvm::StringRef::npos ||
      (OpenBrace != llvm::StringRef::npos && OpenBrace > StatementStart))
    StatementStart = OpenBrace;
  StatementStart =
      StatementStart == llvm::StringRef::npos ? 0 : StatementStart + 1;
  size_t Equals = Body.find('=', StatementStart);
  ASSERT_NE(Equals, llvm::StringRef::npos) << Body.str();
  ASSERT_LT(Equals, Call) << "pe_leaf call is not assigned in its statement:\n"
                          << Body.str();
  llvm::StringRef LHS = Body.slice(StatementStart, Equals);
  LHS = LHS.trim();
  ASSERT_FALSE(LHS.empty()) << Body.str();
  ASSERT_TRUE(std::all_of(
      LHS.begin(), LHS.end(),
      [](char C) {
        return std::isalnum(static_cast<unsigned char>(C)) || C == '_';
      }))
      << "unexpected call-result expression: " << LHS.str();

  size_t CallEnd = Body.find(';', Call);
  ASSERT_NE(CallEnd, llvm::StringRef::npos) << Body.str();
  llvm::StringRef Rest = Body.drop_front(CallEnd + 1);
  bool Stored = false;
  while (!Rest.empty()) {
    auto [Line, Remaining] = Rest.split('\n');
    Rest = Remaining;
    Line = Line.trim();
    llvm::StringRef RHS;
    if (Line.starts_with("*(")) {
      size_t StoreEquals = Line.find('=');
      size_t Semicolon = Line.rfind(';');
      if (StoreEquals == llvm::StringRef::npos ||
          Semicolon == llvm::StringRef::npos || StoreEquals >= Semicolon)
        continue;
      RHS = Line.slice(StoreEquals + 1, Semicolon).trim();
    } else if (Line.starts_with("neverd_mem_store_")) {
      size_t Open = Line.find('(');
      if (Open == llvm::StringRef::npos)
        continue;
      unsigned Depth = 1;
      size_t Comma = llvm::StringRef::npos;
      size_t Close = llvm::StringRef::npos;
      for (size_t I = Open + 1; I < Line.size(); ++I) {
        if (Line[I] == '(')
          ++Depth;
        else if (Line[I] == ')') {
          if (--Depth == 0) {
            Close = I;
            break;
          }
        } else if (Line[I] == ',' && Depth == 1 &&
                   Comma == llvm::StringRef::npos) {
          Comma = I;
        }
      }
      if (Comma == llvm::StringRef::npos || Close == llvm::StringRef::npos)
        continue;
      RHS = Line.slice(Comma + 1, Close).trim();
    } else {
      continue;
    }
    while (RHS.starts_with("(")) {
      size_t Close = RHS.find(')');
      if (Close == llvm::StringRef::npos)
        break;
      llvm::StringRef Cast = RHS.slice(1, Close).trim();
      if (Cast.empty() || !std::all_of(Cast.begin(), Cast.end(), [](char C) {
            return std::isalnum(static_cast<unsigned char>(C)) || C == '_' ||
                   C == '*' || std::isspace(static_cast<unsigned char>(C));
          }))
        break;
      RHS = RHS.drop_front(Close + 1).trim();
    }
    if (RHS == LHS) {
      Stored = true;
      break;
    }
  }
  EXPECT_TRUE(Stored) << "pe_leaf result " << LHS.str()
                      << " is not stored after the call:\n"
                      << Body.str();
}

inline void expectLeafCallUsesParameter(llvm::StringRef Body,
                                 llvm::StringRef Parameter) {
  size_t Call = Body.find("pe_leaf(");
  ASSERT_NE(Call, llvm::StringRef::npos) << Body.str();
  size_t CallEnd = Body.find(';', Call);
  ASSERT_NE(CallEnd, llvm::StringRef::npos) << Body.str();
  llvm::StringRef Statement = Body.slice(Call, CallEnd);
  EXPECT_TRUE(Statement.contains(Parameter))
      << "pe_leaf call does not use " << Parameter.str() << ":\n"
      << Statement.str();
}

inline bool isCIdentifierChar(char C) {
  return std::isalnum(static_cast<unsigned char>(C)) || C == '_';
}

inline size_t findIdentifier(llvm::StringRef Text, llvm::StringRef Name, size_t From) {
  while (true) {
    size_t Pos = Text.find(Name, From);
    if (Pos == llvm::StringRef::npos)
      return Pos;
    bool LeftBoundary = Pos == 0 || !isCIdentifierChar(Text[Pos - 1]);
    size_t End = Pos + Name.size();
    bool RightBoundary = End == Text.size() || !isCIdentifierChar(Text[End]);
    if (LeftBoundary && RightBoundary)
      return Pos;
    From = End;
  }
}

inline void expectNoLocalReadBeforeDefinition(llvm::StringRef Body) {
  struct LocalDecl {
    std::string Name;
    size_t End = 0;
  };
  std::vector<LocalDecl> Locals;
  size_t OpenBrace = Body.find('{');
  ASSERT_NE(OpenBrace, llvm::StringRef::npos) << Body.str();
  size_t LineStart = OpenBrace + 1;
  while (LineStart < Body.size()) {
    size_t LineEnd = Body.find('\n', LineStart);
    if (LineEnd == llvm::StringRef::npos)
      LineEnd = Body.size();
    llvm::StringRef Line = Body.slice(LineStart, LineEnd).trim();
    if (Line.ends_with(";") && !Line.contains('=') && !Line.contains('(')) {
      llvm::StringRef Declaration = Line.drop_back().trim();
      size_t TypeEnd = Declaration.find_first_of(" \t");
      llvm::StringRef Type = Declaration.take_front(TypeEnd);
      if (Type.ends_with("_t") || Type == "int" || Type == "long" ||
          Type == "short" || Type == "char" || Type == "float" ||
          Type == "double") {
        size_t NameEnd = Declaration.size();
        while (NameEnd > 0 && std::isspace(static_cast<unsigned char>(
                                  Declaration[NameEnd - 1])))
          --NameEnd;
        size_t NameStart = NameEnd;
        while (NameStart > 0 && isCIdentifierChar(Declaration[NameStart - 1]))
          --NameStart;
        llvm::StringRef Name = Declaration.slice(NameStart, NameEnd);
        if (!Name.empty())
          Locals.push_back({Name.str(), LineEnd});
      }
    }
    LineStart = LineEnd + 1;
  }

  std::set<llvm::StringRef> DeclaredNames;
  for (const LocalDecl &Local : Locals)
    DeclaredNames.insert(Local.Name);
  std::set<std::string> UndeclaredNames;
  for (size_t Pos = OpenBrace + 1; Pos + 1 < Body.size(); ++Pos) {
    if (Body[Pos] != 'v' ||
        !std::isdigit(static_cast<unsigned char>(Body[Pos + 1])) ||
        (Pos > 0 && isCIdentifierChar(Body[Pos - 1])))
      continue;
    size_t End = Pos + 2;
    while (End < Body.size() && isCIdentifierChar(Body[End]))
      ++End;
    llvm::StringRef Name = Body.slice(Pos, End);
    if (!DeclaredNames.count(Name))
      UndeclaredNames.insert(Name.str());
    Pos = End - 1;
  }

  std::vector<std::string> ReadBeforeDefinition;
  for (const LocalDecl &Local : Locals) {
    size_t Use = findIdentifier(Body, Local.Name, Local.End);
    if (Use == llvm::StringRef::npos)
      continue;
    size_t StatementStart = Body.rfind(';', Use);
    size_t OpenBrace = Body.rfind('{', Use);
    if (StatementStart == llvm::StringRef::npos ||
        (OpenBrace != llvm::StringRef::npos && OpenBrace > StatementStart))
      StatementStart = OpenBrace;
    StatementStart =
        StatementStart == llvm::StringRef::npos ? 0 : StatementStart + 1;
    size_t StatementEnd = Body.find(';', Use);
    size_t Equals = Body.find('=', StatementStart);
    bool IsDefinition = StatementEnd != llvm::StringRef::npos &&
                        Equals < StatementEnd && Use < Equals &&
                        Body.slice(StatementStart, Equals).trim() == Local.Name;
    if (!IsDefinition)
      ReadBeforeDefinition.push_back(Local.Name);
  }

  EXPECT_TRUE(ReadBeforeDefinition.empty())
      << "locals read before definition: "
      << llvm::join(ReadBeforeDefinition, ", ") << "\n"
      << Body.str();
  EXPECT_TRUE(UndeclaredNames.empty())
      << "undeclared locals: " << llvm::join(UndeclaredNames, ", ") << "\n"
      << Body.str();
}

inline void expectFrameBaseInitializedOnce(llvm::StringRef Body) {
  constexpr llvm::StringLiteral Declaration = "uintptr_t frame_base =";
  size_t Init = Body.find(Declaration);
  ASSERT_NE(Init, llvm::StringRef::npos) << Body.str();
  size_t InitEnd = Body.find(';', Init);
  ASSERT_NE(InitEnd, llvm::StringRef::npos) << Body.str();

  std::vector<std::string> Reassignments;
  size_t Use = findIdentifier(Body, "frame_base", InitEnd + 1);
  while (Use != llvm::StringRef::npos) {
    size_t StatementStart = Body.rfind(';', Use);
    size_t OpenBrace = Body.rfind('{', Use);
    if (StatementStart == llvm::StringRef::npos ||
        (OpenBrace != llvm::StringRef::npos && OpenBrace > StatementStart))
      StatementStart = OpenBrace;
    StatementStart =
        StatementStart == llvm::StringRef::npos ? 0 : StatementStart + 1;
    size_t StatementEnd = Body.find(';', Use);
    size_t Equals = Body.find('=', StatementStart);
    if (StatementEnd != llvm::StringRef::npos && Equals < StatementEnd &&
        Use < Equals &&
        Body.slice(StatementStart, Equals).trim() == "frame_base")
      Reassignments.push_back(Body.slice(StatementStart, StatementEnd).str());
    Use = findIdentifier(Body, "frame_base", Use + 10);
  }

  EXPECT_TRUE(Reassignments.empty())
      << "frame_base must remain the immutable entry stack pointer; found: "
      << llvm::join(Reassignments, ", ") << "\n"
      << Body.str();
}

inline std::string readTextFile(const fs::path &Path) {
  std::ifstream In(Path);
  return std::string(std::istreambuf_iterator<char>(In), {});
}

class COFFARMPipeline : public NeverDLiftTest {
protected:
  void expectGeneratedCCompiles(const fs::path &CPath,
                                llvm::StringRef TargetTriple) {
    const fs::path IncludeDir = tmpFile("windows-arm-include");
    fs::create_directories(IncludeDir);
    std::ofstream StringHeader(IncludeDir / "string.h");
    StringHeader << "#include <stddef.h>\n"
                    "void *memcpy(void *, const void *, size_t);\n";
    StringHeader.close();
    ASSERT_TRUE(StringHeader.good());

    RunResult Syntax = exec(
        "clang", {"-std=c11", "-target", TargetTriple.str(), "-ffreestanding",
                  "-I", IncludeDir.string(), "-fsyntax-only", CPath.string()});
    EXPECT_EQ(Syntax.exitCode, 0) << Syntax.err;
  }
};

} // namespace neverd::coff_arm_test

#endif // NEVERD_UNITTESTS_LIFT_FORMAT_COFFARMPIPELINETESTSDETAIL_H
