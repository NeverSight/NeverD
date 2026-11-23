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

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <limits>
#include <vector>

namespace neverd {

llvm::Expected<X64RtmAbortSelection>
X64RtmAbortSelection::read(const llvm::json::Value &V) {
  X64RtmAbortSelection Result;
  if (const auto Flag = V.getAsBoolean()) {
    Result.Kind = *Flag ? Supported : Off;
    return Result;
  }
  if (V.getAsString() == "auto") {
    Result.Kind = Probe;
    return Result;
  }
  const auto *Record = V.getAsObject();
  if (!Record)
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "native_rtm_abort_steps requires a boolean, "
                                   "auto or target CPUID record");
  const auto Word = [&](llvm::StringRef Key) -> std::optional<uint32_t> {
    if (const auto *Value = Record->get(Key)) {
      if (const auto Number = Value->getAsUINT64();
          Number && *Number <= UINT32_MAX)
        return uint32_t(*Number);
      uint32_t Number;
      if (const auto Text = Value->getAsString();
          Text && !Text->empty() && !Text->starts_with('-') &&
          !Text->getAsInteger(0, Number))
        return Number;
    }
    return std::nullopt;
  };
  Result.MaxBasic = Word("max_basic_leaf");
  Result.Leaf7EBX = Word("leaf7_ebx");
  if (!Result.MaxBasic ||
      (*Result.MaxBasic >= 7) != Result.Leaf7EBX.has_value() ||
      Record->size() != 1u + Result.Leaf7EBX.has_value())
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "RTM CPUID requires max_basic_leaf and "
                                   "leaf7_ebx exactly when leaf 7 exists");
  Result.Kind =
      (Result.Leaf7EBX.value_or(0) & (1U << 11)) ? Supported : Unadvertised;
  return Result;
}

llvm::json::Object
X64RtmAbortSelection::report(llvm::json::Array Regions) const {
  llvm::json::Object R{
      {"native_intrinsic", "llvm.x86.rtm.abort.pair"},
      {"regions", std::move(Regions)},
      {"domain", "original execution CPU, outside RTM/HLE, successful "
                 "instruction fetch and no debug trap"},
      {"budget",
       "one translated step per attempted pair; zero on capability refusal"},
      {"selection", Kind == Probe ? "executing-cpu-cpuid"
                    : MaxBasic    ? "captured-cpuid"
                                  : "declared-rtm"},
      {"can_decline", Kind == Probe || Kind == Unadvertised}};
  if (MaxBasic)
    R["max_basic_leaf"] = *MaxBasic;
  if (Leaf7EBX)
    R["leaf7_ebx"] = *Leaf7EBX;
  return R;
}

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
void emitCpuHasRtm(llvm::raw_ostream &OS) {
  OS << "static __inline__ __attribute__((__always_inline__)) int "
        "__neverd_x64_cpu_has_rtm(void) {\n"
        "#if __has_builtin(__builtin_ia32_cpuidex)\n"
        "        int info[4];\n"
        "        __builtin_ia32_cpuidex(info,0,0);\n"
        "        if ((uint32_t)info[0] < 7) return 0;\n"
        "        __builtin_ia32_cpuidex(info,7,0);\n"
        "        return ((uint32_t)info[1] >> 11) & 1U;\n"
        "#else\n"
        "        uint32_t a,b,c,d;\n"
        "        __asm__ volatile(\"cpuid\" : "
        "\"=a\"(a),\"=b\"(b),\"=c\"(c),\"=d\"(d)\n"
        "                         : \"0\"(0),\"2\"(0) : \"cc\");\n"
        "        if (a < 7) return 0;\n"
        "        __asm__ volatile(\"cpuid\" : "
        "\"=a\"(a),\"=b\"(b),\"=c\"(c),\"=d\"(d)\n"
        "                         : \"0\"(7),\"2\"(0) : \"cc\");\n"
        "        return (b >> 11) & 1U;\n"
        "#endif\n}\n";
}

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

X64RtmAbortDispatch::X64RtmAbortDispatch(
    X64RtmAbortSelection::Mode Mode, llvm::ArrayRef<X64RtmAbortStep> Steps) {
  if (Mode == X64RtmAbortSelection::Probe)
    for (const auto &Step : Steps)
      Sites.insert(Step.Region.Address);
}

void X64RtmAbortDispatch::declare(
    llvm::raw_ostream &OS,
    llvm::ArrayRef<analysis::X64ContinuationInstructionSource> Units) const {
  if (!Sites.empty() && llvm::any_of(Units, [&](const auto &Unit) {
        return Sites.contains(Unit.Address);
      }))
    OS << "    unsigned rtm_advertised = 0;\n";
}

void X64RtmAbortDispatch::emit(llvm::raw_ostream &OS,
                               llvm::StringRef Name) const {
  OS << "status = " << Name << "_checked((uint8_t *)state, &rtm_advertised);";
}

llvm::json::Array emitX64RtmAbortSteps(llvm::raw_ostream &OS,
                                       llvm::ArrayRef<X64RtmAbortStep> Steps,
                                       X64RtmAbortSelection::Mode Mode) {
  using Exit = analysis::X64ContinuationExitV1;
  llvm::json::Array Receipts;
  if (Mode == X64RtmAbortSelection::Probe)
    emitCpuHasRtm(OS);
  for (const auto &[Name, Region] : Steps) {
    Receipts.push_back(receipt(Region));
    if (Mode == X64RtmAbortSelection::Unadvertised)
      continue;
    const bool Probe = Mode == X64RtmAbortSelection::Probe;
    if (Probe)
      OS << "static uint64_t " << Name
         << "_checked(uint8_t *raw, unsigned *advertised)";
    else
      OS << "uint64_t " << Name << "(uint8_t *raw)";
    OS << " {\n        uint64_t check=" << Name
       << "_guard(raw);\n"
          "        if (check!="
       << uint64_t(Exit::NativeInstruction)
       << ") return check;\n"
          "        neverd_x64_state_v6 *state=(neverd_x64_state_v6 *)(void "
          "*)raw;\n"
          "        uint32_t status;\n";
    if (Probe)
      OS << "        if (!*advertised) {\n"
            "            if (!__neverd_x64_cpu_has_rtm()) return check;\n"
            "            *advertised = 1;\n        }\n";
    emitAbortPair(OS, Region);
    OS << "        return " << uint64_t(Exit::Jump) << ";\n}\n";
    if (Probe)
      OS << "uint64_t " << Name << "(uint8_t *raw) {\n"
         << "    unsigned advertised = 0;\n    return " << Name
         << "_checked(raw, &advertised);\n}\n";
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
  if (!Regions.empty())
    emitCpuHasRtm(OS);
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
          "        if (!__neverd_x64_cpu_has_rtm()) return 0;\n";
    emitAbortPair(OS, Region);
    OS << "        return 1;\n    }\n";
    Receipts.push_back(receipt(Region));
  }
  OS << "    default: return 0;\n    }\n}\n";
  return Receipts;
}

} // namespace neverd
