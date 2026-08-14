//===- PatternParser.cpp - FLIRT .pat text format parser -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sigs/PatternParser.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/MemoryBuffer.h"

#include <string>

using namespace neverd::sigs;

namespace {

bool isIgnorablePatternLine(llvm::StringRef Line) {
  Line = Line.trim();
  return Line.empty() || Line.starts_with(";") || Line.starts_with("#") ||
         Line == "---";
}

llvm::Expected<std::vector<PatternModule>>
parsePatternTextStrict(llvm::StringRef Text) {
  std::vector<PatternModule> Modules;
  llvm::SmallVector<llvm::StringRef, 0> Lines;
  Text.split(Lines, '\n');

  for (size_t I = 0; I < Lines.size(); ++I) {
    if (isIgnorablePatternLine(Lines[I]))
      continue;
    auto ModOrErr = PatternParser::parseLine(Lines[I]);
    if (!ModOrErr)
      return llvm::make_error<llvm::StringError>(
          "pattern line " + std::to_string(I + 1) + ": " +
              llvm::toString(ModOrErr.takeError()),
          llvm::inconvertibleErrorCode());
    Modules.push_back(std::move(*ModOrErr));
  }
  return Modules;
}

} // namespace

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
  llvm::SplitString(Line, Tokens);

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
  if (Mod.TotalLen == 0)
    return llvm::make_error<llvm::StringError>("total length must be non-zero",
                                               llvm::inconvertibleErrorCode());
  if (Mod.CRCLen != 0 && (Mod.LeadingBytes.size() > Mod.TotalLen ||
                          Mod.CRCLen > Mod.TotalLen - Mod.LeadingBytes.size()))
    return llvm::make_error<llvm::StringError>(
        "CRC range is outside total length", llvm::inconvertibleErrorCode());

  // Remaining tokens form :offset/name pairs followed by at most one tail.
  bool SawTail = false;
  for (size_t I = 4; I < Tokens.size(); ++I) {
    if (Tokens[I].starts_with("^"))
      return llvm::make_error<llvm::StringError>(
          "reference constraint is not supported: " + Tokens[I].str(),
          llvm::inconvertibleErrorCode());
    if (Tokens[I].starts_with(":")) {
      if (SawTail)
        return llvm::make_error<llvm::StringError>(
            "public name follows the tail pattern",
            llvm::inconvertibleErrorCode());
      auto OffStr = Tokens[I].drop_front(1);
      unsigned Off;
      if (OffStr.empty() || OffStr.getAsInteger(16, Off))
        return llvm::make_error<llvm::StringError>(
            "invalid public name offset: " + Tokens[I].str(),
            llvm::inconvertibleErrorCode());
      if (Off >= Mod.TotalLen)
        return llvm::make_error<llvm::StringError>(
            "public name offset is outside total length",
            llvm::inconvertibleErrorCode());
      if (I + 1 >= Tokens.size() || Tokens[I + 1].starts_with(":") ||
          Tokens[I + 1].starts_with("^") || Tokens[I + 1].starts_with(".."))
        return llvm::make_error<llvm::StringError>(
            "public name is missing after offset: " + Tokens[I].str(),
            llvm::inconvertibleErrorCode());

      FuncRef Ref;
      Ref.Offset = Off;
      Ref.Name = Tokens[++I].str();
      Mod.PublicNames.push_back(std::move(Ref));
      continue;
    }

    if (SawTail)
      return llvm::make_error<llvm::StringError>(
          "unexpected field after the tail pattern: " + Tokens[I].str(),
          llvm::inconvertibleErrorCode());
    auto TailOrErr = parseHexPattern(Tokens[I]);
    if (!TailOrErr)
      return llvm::make_error<llvm::StringError>(
          "invalid tail pattern: " + llvm::toString(TailOrErr.takeError()),
          llvm::inconvertibleErrorCode());
    Mod.TailBytes = std::move(*TailOrErr);
    SawTail = true;
  }

  if (Mod.PublicNames.empty())
    return llvm::make_error<llvm::StringError>("no public names found",
                                               llvm::inconvertibleErrorCode());

  return Mod;
}

llvm::Expected<std::vector<PatternModule>>
PatternParser::parseText(llvm::StringRef Text) {
  return parsePatternTextStrict(Text);
}

llvm::Expected<std::vector<PatternModule>>
PatternParser::parseFile(const std::filesystem::path &Path) {
  auto BufOrErr = llvm::MemoryBuffer::getFile(Path.string());
  if (!BufOrErr)
    return llvm::make_error<llvm::StringError>("cannot open pattern file: " +
                                                   Path.string(),
                                               llvm::inconvertibleErrorCode());

  return parseText((*BufOrErr)->getBuffer());
}
