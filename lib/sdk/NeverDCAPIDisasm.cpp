//===- NeverDCAPIDisasm.cpp - C API: disassembly --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Disassembly functions: JSON array output and annotated text output.
///
//===----------------------------------------------------------------------===//

#include "SessionImpl.h"

#include "neverd/evm/Analyzer.h"

#include "llvm/Support/Format.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <cctype>

using namespace neverd;
using namespace neverd::sdk;

namespace {

inline constexpr size_t kShortInstructionByteColumnWidth = 18;

std::string evmBytes(const evm::LowInstruction &Instruction) {
  std::string Bytes;
  for (uint8_t Byte : Instruction.Encoding)
    Bytes += llvm::utohexstr(Byte, /*LowerCase=*/true, evm::kHexDigitsPerByte);
  return Bytes;
}

} // namespace

// ===--------------------------------------------------------------------===//
// Disassembly (JSON array)
// ===--------------------------------------------------------------------===//

const char *neverd_disasm_json(neverd_session_t Sess, neverd_va_t Addr,
                               int MaxInsns) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return dupStr(std::string("[]"));
  }

  if (S->Img.Arch == Arch::EVM) {
    if (!S->ensurePipeline() || !S->PipeResult.EVM)
      return dupStr(std::string("[]"));
    llvm::json::Array EVMInstructions;
    int Count = 0;
    for (const auto &Instruction : S->PipeResult.EVM->Low.Instructions) {
      if (Instruction.PC < Addr)
        continue;
      if (MaxInsns > 0 && Count >= MaxInsns)
        break;
      llvm::json::Object Object;
      Object["addr"] = vaHex(Instruction.PC);
      Object["size"] = static_cast<int64_t>(Instruction.Encoding.size());
      Object["mnemonic"] = std::string(Instruction.Info.Name);
      Object["op_str"] = evm::formatImmediate(Instruction);
      Object["bytes"] = evmBytes(Instruction);
      EVMInstructions.push_back(std::move(Object));
      ++Count;
    }
    return dupStr(jsonToString(llvm::json::Value(std::move(EVMInstructions))));
  }

  llvm::json::Array Arr;
  va_t Cur = Addr;
  uint64_t Span = 0;
  for (const auto &F : S->Functions) {
    if (F.Entry == Addr) {
      Span = F.Size;
      break;
    }
  }
  uint64_t Limit =
      MaxInsns > 0 ? static_cast<uint64_t>(MaxInsns) : (Span > 0 ? Span : 256);

  for (uint64_t I = 0; I < Limit; ++I) {
    if (Cur < Addr)
      break;
    const Segment *Seg = S->Img.getSegmentFor(Cur);
    if (!Seg || !Seg->isExecutable())
      break;

    uint64_t Consumed = Cur - Addr;
    if (Span > 0 && Consumed >= Span)
      break;

    uint64_t Off64 = Cur - Seg->VA;
    if (Off64 >= Seg->Data.size())
      break;
    size_t Off = static_cast<size_t>(Off64);
    uint64_t Avail64 = std::min<uint64_t>(
        16, std::min<uint64_t>(Seg->Size - Off64, Seg->Data.size() - Off));
    if (Span > 0)
      Avail64 = std::min(Avail64, Span - Consumed);
    if (Avail64 == 0)
      break;
    const uint8_t *Bytes = Seg->Data.data() + Off;

    DecodedInsn DI;
    int Sz = S->Dec.decodeOne(Bytes, static_cast<size_t>(Avail64), Cur, DI);
    if (Sz <= 0)
      break;

    std::string BytesHex;
    for (int J = 0; J < Sz; ++J) {
      char Buf[4];
      snprintf(Buf, sizeof(Buf), "%02x", Bytes[J]);
      BytesHex += Buf;
    }

    llvm::json::Object Obj;
    Obj["addr"] = vaHex(Cur);
    Obj["size"] = Sz;
    Obj["mnemonic"] = std::string(DI.Raw ? DI.Raw->mnemonic : "");
    Obj["op_str"] = std::string(DI.Raw ? DI.Raw->op_str : "");
    Obj["bytes"] = BytesHex;

    Arr.push_back(std::move(Obj));
    if (static_cast<uint64_t>(Sz) > InvalidVA - Cur)
      break;
    Cur += Sz;
  }

  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

// ===--------------------------------------------------------------------===//
// Disassembly (annotated text)
// ===--------------------------------------------------------------------===//

const char *neverd_disasm_text(neverd_session_t Sess,
                               const char *FuncNameOrAddr, int Annotate) {
  auto *S = static_cast<Session *>(Sess);
  if (!S || !S->Loaded)
    return nullptr;

  if (S->Img.Arch == Arch::EVM) {
    if (!S->ensurePipeline() || !S->PipeResult.EVM)
      return nullptr;
    std::string Buffer;
    llvm::raw_string_ostream OS(Buffer);
    OS << "; " << kEVMEntrySymbolName << " (0x"
       << llvm::utohexstr(evm::kEntryPC) << ", " << S->Img.Raw.size()
       << " bytes)\n";
    for (const auto &Instruction : S->PipeResult.EVM->Low.Instructions) {
      OS << "  0x" << llvm::utohexstr(Instruction.PC) << "  ";
      const std::string Bytes = evmBytes(Instruction);
      OS << Bytes;
      if (Bytes.size() < kShortInstructionByteColumnWidth)
        OS.indent(kShortInstructionByteColumnWidth - Bytes.size());
      OS << " " << Instruction.Info.Name;
      const std::string Immediate = evm::formatImmediate(Instruction);
      if (!Immediate.empty())
        OS << " " << Immediate;
      OS << "\n";
    }
    return dupStr(Buffer);
  }

  const Symbol *Sym = nullptr;
  std::string FN(FuncNameOrAddr ? FuncNameOrAddr : "");
  if (FN.size() > 2 && (FN.substr(0, 2) == "0x" || FN.substr(0, 2) == "0X")) {
    llvm::StringRef Ref(FN);
    Ref = Ref.drop_front(2);
    va_t Addr = 0;
    if (!Ref.getAsInteger(16, Addr))
      Sym = S->Img.findSymbolAt(Addr);
  } else {
    Sym = S->Img.findSymbol(FN);
  }
  if (!Sym)
    return nullptr;

  const Segment *Seg = nullptr;
  for (const auto &Sg : S->Img.Segments)
    if (Sg.contains(Sym->Addr)) {
      Seg = &Sg;
      break;
    }
  if (!Seg)
    return nullptr;

  if (!S->Dec.init(S->Img.Arch, S->Img.Mode))
    return nullptr;

  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  OS << "; " << Sym->Name << " (0x" << llvm::utohexstr(Sym->Addr) << ", "
     << Sym->Size << " bytes)\n";

  va_t Addr = Sym->Addr;
  uint64_t Span = Sym->Size > 0 ? Sym->Size : 0x100;
  while (Addr >= Sym->Addr && Addr - Sym->Addr < Span && Seg->contains(Addr)) {
    uint64_t Off64 = Addr - Seg->VA;
    // contains() only checks the VA span (Seg->Size), which can exceed the
    // materialized bytes; guard so Remain does not underflow into a huge
    // length.
    if (Off64 >= Seg->Data.size())
      break;
    size_t Off = static_cast<size_t>(Off64);
    const uint8_t *Bytes = Seg->Data.data() + Off;
    size_t Remain = static_cast<size_t>(std::min<uint64_t>(
        Seg->Size - Off64,
        std::min<uint64_t>(Seg->Data.size() - Off, Span - (Addr - Sym->Addr))));
    DecodedInsn Insn;
    int Sz = S->Dec.decodeOne(Bytes, Remain, Addr, Insn);
    if (Sz <= 0)
      break;
    OS << "  0x" << llvm::utohexstr(Addr) << "  ";
    for (int I = 0; I < Sz && I < 8; ++I)
      OS << llvm::format("%02x ", Bytes[I]);
    for (int I = Sz; I < 8; ++I)
      OS << "   ";
    OS << " " << (Insn.Raw ? Insn.Raw->mnemonic : "");
    if (Insn.Raw && Insn.Raw->op_str[0])
      OS << "\t" << Insn.Raw->op_str;

    if (Annotate && Insn.Raw && Insn.Raw->op_str[0]) {
      std::string OpStr(Insn.Raw->op_str);
      auto HexPos = OpStr.find("0x");
      if (HexPos != std::string::npos) {
        size_t HexBegin = HexPos + 2;
        size_t HexEnd = HexBegin;
        while (HexEnd < OpStr.size() &&
               std::isxdigit(static_cast<unsigned char>(OpStr[HexEnd])))
          ++HexEnd;
        va_t OpAddr = 0;
        llvm::StringRef HexDigits(OpStr.data() + HexBegin, HexEnd - HexBegin);
        if (HexDigits.empty() || HexDigits.getAsInteger(16, OpAddr))
          OpAddr = 0;
        if (OpAddr > 0 && OpAddr != Addr) {
          const char *Ref = neverd_resolve_addr(Sess, OpAddr);
          if (Ref) {
            auto Parsed = llvm::json::parse(Ref);
            if (Parsed) {
              if (auto *RObj = Parsed->getAsObject()) {
                auto Type = RObj->getString("type").value_or("");
                if (Type == "function")
                  OS << " ; " << RObj->getString("name").value_or("");
                else if (Type == "import")
                  OS << " ; [import] " << RObj->getString("name").value_or("");
                else if (Type == "export")
                  OS << " ; [export] " << RObj->getString("name").value_or("");
                else if (Type == "string")
                  OS << " ; \"" << RObj->getString("value").value_or("")
                     << "\"";
              }
            }
            neverd_free_string(Ref);
          }
        }
      }
    }
    OS << "\n";
    if (static_cast<uint64_t>(Sz) > InvalidVA - Addr)
      break;
    Addr += Sz;
  }
  return dupStr(Buf);
}
