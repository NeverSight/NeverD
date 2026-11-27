//===- Recovery.cpp - Shared Lua artifact report ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Dispatches bounded offline readers and preserves source bytes and failure
/// evidence.
//===----------------------------------------------------------------------===//

#include "Chunk.h"

#include "neverd/support/SHA256.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <climits>
#include <filesystem>
#include <fstream>

using namespace llvm;
using namespace neverd::lua;
namespace {
json::Object report(StringRef Profile) {
  return json::Object{
      {"schema", "language-recovery-v1"},
      {"language", "Lua"},
      {"profile",
       json::isUTF8(Profile) ? Profile.str() : json::fixUTF8(Profile)},
      {"status", "unsupported"},
      {"evidence", json::Object{{"producer_revision", nullptr}}},
      {"root", nullptr},
      {"functions", json::Array{}},
      {"diagnostics", json::Array{}}};
}
void failed(json::Object &Result, StringRef Status, StringRef Why,
            std::optional<size_t> Offset = {}) {
  Result["status"] = Status.str();
  json::Object D{{"message", Why.str()}};
  if (Offset)
    D["offset"] = *Offset;
  Result.getArray("diagnostics")->push_back(std::move(D));
}
bool sourceText(StringRef B) {
  return json::isUTF8(B) && llvm::all_of(B, [](unsigned char C) {
           return C >= 32 || C == '\n' || C == '\r' || C == '\t' || C == '\f' ||
                  C == '\v';
         });
}
} // namespace
json::Object neverd::lua::recover(StringRef B, StringRef Ext, StringRef Profile,
                                  Cancel Stop) {
  auto Result = report(Profile);
  auto &E = *Result.getObject("evidence");
  Budget Job{Stop};
  try {
    Job.check(0);
    if (B.size() > limits::kInputBytes)
      throw Failure{"budget_exceeded", "Lua input byte budget exceeded", 0};
    Result["input_sha256"] = toHex(sha256(arrayRefFromStringRef(B)), true);
    Result["input_bytes"] = B.size();
    bool Auto = Profile == "auto", Puc = Profile == "puc",
         Jit = Profile == "luajit", Luau = Profile == "luau-0.659";
    if (!Auto && !Puc && !Jit && !Luau)
      unsupported(0, "Unknown Lua recovery profile");
    bool PucHeader = B.starts_with("\x1bLua"),
         JitHeader = B.starts_with("\x1bLJ");
    bool LuaSource = Ext.equals_insensitive(".lua"),
         LuauSource = Ext.equals_insensitive(".luau");
    bool Source =
        (LuaSource || LuauSource) && !PucHeader && !JitHeader && sourceText(B);
    if (Source) {
      if (LuauSource && (Puc || Jit))
        unsupported(0, "Source extension conflicts with selected dialect");
      E["dialect"] = Luau || LuauSource ? "Luau" : Jit ? "LuaJIT" : "Lua";
      E["identification"] = "source extension and UTF-8 text";
      Result["status"] = "extracted_source";
      Result["root"] = 0;
      Result.getArray("functions")
          ->push_back(json::Object{{"id", 0},
                                   {"name", nullptr},
                                   {"offset", 0},
                                   {"end_offset", B.size()},
                                   {"source", B.str()},
                                   {"status", "extracted_source"},
                                   {"instructions", json::Array{}},
                                   {"diagnostics", json::Array{}}});
      return Result;
    }
    if ((PucHeader && !(Puc || Auto)) || (JitHeader && !(Jit || Auto)))
      unsupported(0, "Chunk signature conflicts with selected profile");
    if (!PucHeader && !JitHeader && !Luau) {
      if (!B.empty() && uint8_t(B.front()) == 27 &&
          (StringRef("\x1bLua").starts_with(B) ||
           StringRef("\x1bLJ").starts_with(B)))
        malformed(B.size(), "Truncated Lua chunk signature");
      unsupported(0, "Unknown Lua artifact. Upstream Luau bytecode requires "
                     "profile luau-0.659");
    }
    Reader R{B, Job};
    Chunk C{Job,
            PucHeader   ? Family::Puc
            : JitHeader ? Family::Jit
                        : Family::Luau,
            E};
    E["family"] = PucHeader ? "PUC Lua" : JitHeader ? "LuaJIT" : "Luau";
    E["identification"] =
        Luau ? "explicit upstream compiler profile" : "binary signature";
    try {
      if (PucHeader)
        readPuc(C, R);
      else if (JitHeader)
        readJit(C, R);
      else
        readLuau(C, R);
    } catch (const Failure &F) {
      if (StringRef(F.Status) == "malformed" ||
          StringRef(F.Status) == "unsupported") {
        // Only complete records survive a failed envelope. No entry prototype
        // is published when the parser could not establish the whole tree.
        C.Root = UINT_MAX;
        render(C, Result);
        Result["root"] = nullptr;
      }
      throw;
    }
    render(C, Result);
  } catch (const Failure &F) {
    failed(Result, F.Status, F.Message, F.Offset);
  }
  return Result;
}
json::Object neverd::lua::recoverFile(StringRef Path, StringRef Profile,
                                      Cancel Stop) {
  auto Result = report(Profile);
  auto File = std::filesystem::path(std::u8string(Path.begin(), Path.end()));
  std::error_code EC;
  auto Size = std::filesystem::file_size(File, EC);
  if (EC) {
    failed(Result, "io_error", "Cannot read a regular input file");
    return Result;
  }
  if (Size > limits::kInputBytes) {
    failed(Result, "budget_exceeded", "Lua input byte budget exceeded");
    return Result;
  }
  std::ifstream In(File, std::ios::binary);
  std::string Data(Size, '\0');
  if (!In.read(Data.data(), Size) ||
      In.peek() != std::char_traits<char>::eof() || In.bad()) {
    failed(Result, "io_error", "Cannot read a stable input extent");
    return Result;
  }
  return recover(Data, sys::path::extension(Path), Profile, Stop);
}

std::string neverd::lua::serializeReport(json::Object Report) {
  std::string Text;
  // Keep the input identity and header when the full inventory cannot travel
  // through both frontends. The item/text budgets also bound serialization.
  json::Value Value(std::move(Report));
  raw_string_ostream(Text) << Value;
  if (Text.size() <= limits::kReportBytes)
    return Text;
  auto &R = *Value.getAsObject();
  R["functions"] = json::Array{};
  R["root"] = nullptr;
  failed(R, "budget_exceeded", "Lua report exceeds export budget");
  Text.clear();
  raw_string_ostream(Text) << Value;
  return Text;
}
