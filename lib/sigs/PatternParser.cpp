//===- PatternParser.cpp - FLIRT .pat text format parser -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sigs/PatternParser.h"

#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <cctype>
#include <fstream>

using namespace neverd::sigs;

bool PatternParser::parseHexByte(llvm::StringRef Hex, uint8_t &Out) {
  if (Hex.size() != 2)
    return false;
  unsigned Val;
  if (Hex.getAsInteger(16, Val))
    return false;
  Out = static_cast<uint8_t>(Val);
  return true;
}

llvm::Expected<std::vector<PatternByte>>
PatternParser::parseHexPattern(llvm::StringRef Pat) {
  std::vector<PatternByte> Result;
  if (Pat.size() % 2 != 0)
    return llvm::make_error<llvm::StringError>("hex pattern has odd length",
                                               llvm::inconvertibleErrorCode());

  for (size_t I = 0; I < Pat.size(); I += 2) {
    auto Pair = Pat.substr(I, 2);
    PatternByte PB;
    if (Pair == "..") {
      PB.IsWildcard = true;
      PB.Value = 0;
    } else {
      if (!parseHexByte(Pair, PB.Value))
        return llvm::make_error<llvm::StringError>(
            "invalid hex byte: " + Pair.str(), llvm::inconvertibleErrorCode());
      PB.IsWildcard = false;
    }
    Result.push_back(PB);
  }
  return Result;
}

llvm::Expected<PatternModule> PatternParser::parseLine(llvm::StringRef Line) {
  Line = Line.trim();
  if (Line.empty() || Line.starts_with(";") || Line.starts_with("#") ||
      Line == "---")
    return llvm::make_error<llvm::StringError>("skip line",
                                               llvm::inconvertibleErrorCode());

  PatternModule Mod;

  // Split into tokens by whitespace.
  llvm::SmallVector<llvm::StringRef, 16> Tokens;
  Line.split(Tokens, ' ', /*MaxSplit=*/-1, /*KeepEmpty=*/false);

  if (Tokens.size() < 4)
    return llvm::make_error<llvm::StringError>("too few fields in pattern line",
                                               llvm::inconvertibleErrorCode());

  // Token 0: hex pattern (leading bytes with .. wildcards).
  auto LeadingOrErr = parseHexPattern(Tokens[0]);
  if (!LeadingOrErr)
    return LeadingOrErr.takeError();
  Mod.LeadingBytes = std::move(*LeadingOrErr);

  // Token 1: CRC length (hex byte).
  unsigned CRCLen;
  if (Tokens[1].getAsInteger(16, CRCLen) || CRCLen > 0xFFu)
    return llvm::make_error<llvm::StringError>("invalid CRC length: " +
                                                   Tokens[1].str(),
                                               llvm::inconvertibleErrorCode());
  Mod.CRCLen = static_cast<uint8_t>(CRCLen);

  // Token 2: CRC16 value (hex).
  unsigned CRC16Val;
  if (Tokens[2].getAsInteger(16, CRC16Val) || CRC16Val > 0xFFFFu)
    return llvm::make_error<llvm::StringError>(
        "invalid CRC16: " + Tokens[2].str(), llvm::inconvertibleErrorCode());
  Mod.CRC16 = static_cast<uint16_t>(CRC16Val);

  // Token 3: total length (hex).
  unsigned TotalLen;
  if (Tokens[3].getAsInteger(16, TotalLen))
    return llvm::make_error<llvm::StringError>("invalid total length: " +
                                                   Tokens[3].str(),
                                               llvm::inconvertibleErrorCode());
  Mod.TotalLen = TotalLen;

  // Remaining tokens: :offset name pairs and optional tail bytes.
  for (size_t I = 4; I < Tokens.size(); ++I) {
    if (Tokens[I].starts_with(":")) {
      // :XXXX name
      auto OffStr = Tokens[I].drop_front(1);
      unsigned Off;
      if (OffStr.getAsInteger(16, Off))
        continue;
      if (I + 1 < Tokens.size() && !Tokens[I + 1].starts_with(":") &&
          !Tokens[I + 1].starts_with("..")) {
        FuncRef Ref;
        Ref.Offset = Off;
        Ref.Name = Tokens[I + 1].str();
        Mod.PublicNames.push_back(std::move(Ref));
        ++I;
      }
    } else if (Tokens[I].starts_with("..") ||
               (Tokens[I].size() >= 2 &&
                std::isxdigit(
                    static_cast<unsigned char>(Tokens[I][0])) &&
                std::isxdigit(
                    static_cast<unsigned char>(Tokens[I][1])))) {
      // Tail bytes pattern (after the public names).
      auto TailOrErr = parseHexPattern(Tokens[I]);
      if (TailOrErr)
        Mod.TailBytes = std::move(*TailOrErr);
    }
  }

  if (Mod.PublicNames.empty())
    return llvm::make_error<llvm::StringError>("no public names found",
                                               llvm::inconvertibleErrorCode());

  return Mod;
}

llvm::Expected<std::vector<PatternModule>>
PatternParser::parseFile(const std::filesystem::path &Path) {
  auto BufOrErr = llvm::MemoryBuffer::getFile(Path.string());
  if (!BufOrErr)
    return llvm::make_error<llvm::StringError>("cannot open pattern file: " +
                                                   Path.string(),
                                               llvm::inconvertibleErrorCode());

  std::vector<PatternModule> Modules;
  llvm::StringRef Content = (*BufOrErr)->getBuffer();

  llvm::SmallVector<llvm::StringRef, 0> Lines;
  Content.split(Lines, '\n');

  for (const auto &Line : Lines) {
    auto ModOrErr = parseLine(Line);
    if (!ModOrErr) {
      llvm::consumeError(ModOrErr.takeError());
      continue;
    }
    Modules.push_back(std::move(*ModOrErr));
  }

  return Modules;
}
