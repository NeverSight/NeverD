//===- X64RtmAbortRegions.cpp - Bound immediate RTM aborts ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep the complete native transaction in one compiler operation and transport
/// its actual abort status without guessing an outcome or rolling back C.
//===----------------------------------------------------------------------===//

#include "X64RtmAbortRegions.h"

#include "X64RtmRegions.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/decode/X86/X86TransactionEncoding.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <limits>
#include <vector>

namespace neverd {

std::optional<X64RtmAbortRegion> readX64RtmAbortRegion(const BinaryImage &Image,
                                                       va_t Address,
                                                       unsigned AddressBits) {
  if (Image.Arch != Arch::X64 || (AddressBits != 48 && AddressBits != 57))
    return std::nullopt;
  const auto Read =
      [&](va_t PC,
          X86TransactionKind Kind) -> std::optional<llvm::ArrayRef<uint8_t>> {
    for (unsigned Size = 3; Size <= x86::kMaxInstructionLen; ++Size) {
      if (PC > std::numeric_limits<va_t>::max() - (Size - 1))
        return std::nullopt;
      const auto *Bytes = Image.readVA(PC, Size);
      if (!Bytes || !Image.isCodeAddress(PC) ||
          !Image.isCodeAddress(PC + Size - 1))
        return std::nullopt;
      const auto E = x86TransactionEncoding({Bytes, Size}, PC, true);
      if (E)
        return E->Kind == Kind ? std::optional(llvm::ArrayRef(Bytes, Size))
                               : std::nullopt;
    }
    return std::nullopt;
  };
  const auto Begin = Read(Address, X86TransactionKind::Begin);
  if (!Begin || Address > std::numeric_limits<va_t>::max() - Begin->size())
    return std::nullopt;
  const auto Fallback = x64RtmFallback(*Begin, Address, AddressBits);
  const auto Abort = Read(Address + Begin->size(), X86TransactionKind::Abort);
  if (!Fallback || !Abort)
    return std::nullopt;
  X64RtmAbortRegion Region{Address, *Fallback, {}};
  Region.Bytes.append(Begin->begin(), Begin->end());
  Region.Bytes.append(Abort->begin(), Abort->end());
  return Region;
}

namespace {
/// Both owners issue the entire transaction in one compiler operation. EAX
/// is the actual result, including an abort before XABORT is reached.
void emitAbortPair(llvm::raw_ostream &OS, const X64RtmAbortRegion &Region) {
  const auto Code = unsigned(Region.Bytes.back());
  OS << "#if __has_builtin(__builtin_ia32_rtm_abort_pair)\n"
        "        status=__builtin_ia32_rtm_abort_pair("
     << Code
     << ");\n#else\n"
        "        __asm__ volatile(\"xbegin 1f; xabort $"
     << Code
     << "; 1:\" : \"=a\"(status) : : \"memory\",\"cc\");\n#endif\n"
        "        state->GPR[0]=status;\n        state->PC=0x"
     << llvm::utohexstr(Region.Fallback) << "ULL;\n";
}

llvm::json::Object receipt(const X64RtmAbortRegion &Region) {
  return llvm::json::Object{
      {"pc", "0x" + llvm::utohexstr(Region.Address)},
      {"bytes", llvm::toHex(Region.Bytes)},
      {"fallback", "0x" + llvm::utohexstr(Region.Fallback)},
      {"abort_code", unsigned(Region.Bytes.back())}};
}
} // namespace

llvm::json::Array emitX64RtmAbortSteps(llvm::raw_ostream &OS,
                                       llvm::ArrayRef<X64RtmAbortStep> Steps) {
  using Exit = analysis::X64ContinuationExitV1;
  llvm::json::Array Receipts;
  for (const auto &[Name, Region] : Steps) {
    OS << "uint64_t " << Name
       << "(uint8_t *raw) {\n"
          "        uint64_t check="
       << Name
       << "_guard(raw);\n"
          "        if (check!="
       << uint64_t(Exit::NativeInstruction)
       << ") return check;\n"
          "        neverd_x64_state_v6 *state=(neverd_x64_state_v6 *)(void "
          "*)raw;\n"
          "        uint32_t status;\n";
    emitAbortPair(OS, Region);
    OS << "        return " << uint64_t(Exit::Jump) << ";\n}\n";
    Receipts.push_back(receipt(Region));
  }
  return Receipts;
}

llvm::json::Array emitX64RtmAbortEnvironment(
    llvm::raw_ostream &OS, const BinaryImage &Image,
    llvm::ArrayRef<analysis::X64ContinuationInstructionSource> Units,
    unsigned AddressBits) {
  std::vector<X64RtmAbortRegion> Regions;
  for (const auto &Unit : Units)
    if (Unit.RequiresNativeExecution &&
        x86TransactionBeginEncoding(Unit.Bytes, Unit.Address, true))
      if (auto Region = readX64RtmAbortRegion(Image, Unit.Address, AddressBits))
        Regions.push_back(*Region);
  llvm::json::Array Receipts;
  OS << R"C(
/* Explicit immediate-abort RTM environment, for use with neverd_x64_run_v6.
 * Invoke outside RTM/HLE in the declared nonfaulting, no-debug-trap domain.
 * The complete pair runs in one native operation; no transaction spans a
 * C return or a translated step. Actual EAX status is zero-extended into RAX.
 * One environment budget unit covers the pair, including an early abort.
 * Without CPUID RTM the provider declines without changing explicit state. */
int neverd_x64_rtm_abort_environment_v6(neverd_x64_state_v6 *state,
                                       uint64_t boundary, void *context) {
    (void)context;
    if (!state || boundary != 4) return 0;
    switch (state->PC) {
)C";
  for (const auto &Region : Regions) {
    const auto PC = "0x" + llvm::utohexstr(Region.Address);
    OS << "    case " << PC
       << "ULL: {\n"
          "        uint32_t status;\n"
          "#if __has_builtin(__builtin_ia32_rtm_abort_pair) && "
          "__has_builtin(__builtin_ia32_cpuidex)\n"
          "        int info[4];\n"
          "        __builtin_ia32_cpuidex(info,0,0);\n"
          "        if ((uint32_t)info[0] < 7) return 0;\n"
          "        __builtin_ia32_cpuidex(info,7,0);\n"
          "        if (!((uint32_t)info[1] & (1U << 11))) return 0;\n"
          "#else\n"
          "        uint32_t a,b,c,d;\n"
          "        __asm__ volatile(\"cpuid\" : "
          "\"=a\"(a),\"=b\"(b),\"=c\"(c),\"=d\"(d)\n"
          "                         : \"0\"(0),\"2\"(0) : \"cc\");\n"
          "        if (a < 7) return 0;\n"
          "        __asm__ volatile(\"cpuid\" : "
          "\"=a\"(a),\"=b\"(b),\"=c\"(c),\"=d\"(d)\n"
          "                         : \"0\"(7),\"2\"(0) : \"cc\");\n"
          "        if (!(b & (1U << 11))) return 0;\n"
          "#endif\n";
    emitAbortPair(OS, Region);
    OS << "        return 1;\n    }\n";
    Receipts.push_back(receipt(Region));
  }
  OS << "    default: return 0;\n    }\n}\n";
  return Receipts;
}

} // namespace neverd
