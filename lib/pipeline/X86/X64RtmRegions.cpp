//===- X64RtmRegions.cpp - Native RTM region ownership -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One live outer checkpoint owns explicit state and original ordinary RAM.
//===----------------------------------------------------------------------===//

#include "X64RtmRegions.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/decode/X86/X86TransactionEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <string>

namespace neverd {

std::optional<va_t> x64RtmFallback(llvm::ArrayRef<uint8_t> Bytes, va_t Address,
                                   unsigned AddressBits) {
  if (AddressBits != 48 && AddressBits != 57)
    return std::nullopt;
  const auto Begin = x86TransactionBeginEncoding(Bytes, Address, true);
  if (!Begin)
    return std::nullopt;
  const uint64_t Sign = uint64_t(1) << (AddressBits - 1);
  const uint64_t Canonical =
      (Begin->Target & (Sign - 1)) | ((Begin->Target & Sign) ? ~(Sign - 1) : 0);
  if (Begin->Target != Canonical)
    return std::nullopt;
  return Begin->Target;
}

llvm::Expected<llvm::json::Object> emitX64RtmEnvironment(
    llvm::raw_ostream &OS,
    llvm::ArrayRef<analysis::X64ContinuationInstructionSource> Units,
    unsigned AddressBits, bool FollowInternalCalls) {
  using Exit = analysis::X64ContinuationExitV1;
  const auto Hex = [](va_t PC) { return "0x" + llvm::utohexstr(PC); };
  using Transaction = X86TransactionKind;
  // A native XABORT is the rollback channel. Its immediate must be disjoint
  // from every selected original XABORT, including overlapping code streams.
  // Never consume an original status as a provider refusal.
  std::array<bool, 256> AbortCodes{};
  for (const auto &Unit : Units)
    if (const auto E = x86TransactionEncoding(Unit.Bytes, Unit.Address, true);
        E && E->Kind == Transaction::Abort)
      AbortCodes[E->AbortCode] = true;
  const auto Spare = std::find(AbortCodes.begin(), AbortCodes.end(), false);
  if (Spare == AbortCodes.end())
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "native RTM regions need an unused XABORT imm8 for internal rollback; "
        "the selection contains all 256 codes");
  const unsigned Refusal = unsigned(Spare - AbortCodes.begin());
  const auto Abort = "__builtin_ia32_xabort(" + std::to_string(Refusal) + ");";
  llvm::json::Array Begins, Body, Controls, Standalone;
  OS << R"C(
/* The provider owns the live outer transaction until commit or rollback.
 * Invoke outside RTM/HLE, with ordinary RAM disjoint from state, context,
 * code and the compiler frame. No faults, debug traps or asynchronous events.
 * max_instructions bounds one attempt, including XBEGIN/XEND and nested
 * controls. Context is read-only; the outer environment budget counts attempts.
 * An unselected/unsupported path or exhausted attempt rolls back and declines.
 * A real abort publishes actual EAX and the outer original fallback only. */
typedef struct neverd_x64_rtm_context_v6 {
    uint64_t max_instructions;
} neverd_x64_rtm_context_v6;

__attribute__((target("rtm")))
int neverd_x64_rtm_environment_v6(neverd_x64_state_v6 *state,
                                 uint64_t boundary, void *context) {
    if (!state || !context || boundary != 4) return 0;
    uint64_t remaining=((const neverd_x64_rtm_context_v6 *)context)->max_instructions;
    if (!remaining) return 0;
    uint64_t fallback=0, next=0;
    switch (state->PC) {
)C";
  for (const auto &Unit : Units) {
    if (!Unit.RequiresNativeExecution)
      continue;
    const auto Fallback = x64RtmFallback(Unit.Bytes, Unit.Address, AddressBits);
    const auto E = x86TransactionEncoding(Unit.Bytes, Unit.Address, true);
    if (Fallback || (E && (E->Kind == Transaction::Test ||
                           E->Kind == Transaction::Abort))) {
      OS << "    case " << Hex(Unit.Address) << "ULL:\n"
         << "        if (" << Unit.Function.Name
         << "((uint8_t *)state) != " << unsigned(Exit::NativeInstruction)
         << ") return 0;\n";
      if (Fallback) {
        OS << "        fallback=" << Hex(*Fallback) << "ULL;\n";
        Begins.push_back(llvm::json::Object{{"pc", Hex(Unit.Address)},
                                            {"fallback", Hex(*Fallback)}});
      } else {
        Standalone.push_back(Hex(Unit.Address));
      }
      OS << "        next=" << Hex(Unit.Address + Unit.Bytes.size())
         << "ULL; break;\n";
    }
  }
  OS << R"C(    default: return 0;
    }
#if __has_builtin(__builtin_ia32_cpuidex)
    int info[4];
    __builtin_ia32_cpuidex(info,0,0);
    if ((uint32_t)info[0]<7) return 0;
    __builtin_ia32_cpuidex(info,7,0);
    if (!((uint32_t)info[1]&(1U<<11))) return 0;
#else
    uint32_t a,b,c,d;
    __asm__ volatile("cpuid" : "=a"(a),"=b"(b),"=c"(c),"=d"(d)
                     : "0"(0),"2"(0) : "cc");
    if (a<7) return 0;
    __asm__ volatile("cpuid" : "=a"(a),"=b"(b),"=c"(c),"=d"(d)
                     : "0"(7),"2"(0) : "cc");
    if (!(b&(1U<<11))) return 0;
#endif
)C";
  // Outside a transaction XABORT has no architectural effect and XTEST clears
  // the other status flags while setting ZF. XEND still requires #GP delivery
  // and remains outside this nonfaulting provider.
  OS << "    switch (state->PC) {\n";
  for (const auto &Unit : Units) {
    if (!Unit.RequiresNativeExecution)
      continue;
    const auto E = x86TransactionEncoding(Unit.Bytes, Unit.Address, true);
    if (!E || (E->Kind != Transaction::Test && E->Kind != Transaction::Abort))
      continue;
    OS << "    case " << Hex(Unit.Address) << "ULL: {\n";
    if (E->Kind == Transaction::Test)
      OS << "        unsigned active=__builtin_ia32_xtest();\n"
            "        state->RFlags=(state->RFlags&~0x8d5ULL)|"
            "(active ? 0 : 0x40);\n";
    else
      OS << "        __builtin_ia32_xabort(" << unsigned(E->AbortCode)
         << ");\n";
    OS << "        state->PC=next; return 1;\n    }\n";
  }
  OS << "    default: break;\n    }\n"
        "    unsigned status=__builtin_ia32_xbegin();\n"
        "    if (status!=0xffffffffU) {\n";
  OS << "        if ((status&0xff000001U)=="
     << Hex((uint64_t(Refusal) << 24) | 1) << "U) return 0;\n";
  OS << R"C(        state->GPR[0]=status;
        state->PC=fallback;
        return 1;
    }
    uint64_t depth=1;
    state->PC=next;
    --remaining;
    for (;;) {
)C";
  OS << "        if (!remaining) { " << Abort << " }\n"
     << "        --remaining;\n        switch (state->PC) {\n";
  for (const auto &Unit : Units) {
    const auto Bytes = llvm::ArrayRef(Unit.Bytes);
    const bool Begin =
        Unit.RequiresNativeExecution &&
        x64RtmFallback(Bytes, Unit.Address, AddressBits).has_value();
    const auto E = x86TransactionEncoding(Bytes, Unit.Address, true);
    const bool End = E && E->Kind == Transaction::End;
    const bool Test = E && E->Kind == Transaction::Test;
    const bool Cpuid = Bytes == llvm::ArrayRef<uint8_t>({0x0f, 0xa2});
    const bool Pause = Bytes == llvm::ArrayRef<uint8_t>({0xf3, 0x90});
    const bool ExplicitAbort = E && E->Kind == Transaction::Abort;
    if (!Begin && !End && !Test && !Cpuid && !Pause && !ExplicitAbort &&
        !Unit.SupportsRtmBody)
      continue;
    OS << "        case " << Hex(Unit.Address) << "ULL: {\n";
    const auto Advance =
        "            state->PC=" + Hex(Unit.Address + Bytes.size()) + "ULL;\n";
    if (Begin) {
      // An inner abort transfers directly to the still-live outer checkpoint.
      // Its encoded fallback is never selected while nested.
      OS << "            if (__builtin_ia32_xbegin()!=0xffffffffU) { " << Abort
         << " }\n            ++depth;\n"
         << Advance;
    } else if (End) {
      OS << Advance
         << "            __builtin_ia32_xend();\n"
            "            if (!--depth) return 1;\n";
    } else if (Test) {
      OS << "            unsigned active=__builtin_ia32_xtest();\n"
            "            state->RFlags=(state->RFlags&~0x8d5ULL)|"
            "(active ? 0 : 0x40);\n"
         << Advance;
    } else if (ExplicitAbort) {
      OS << "            __builtin_ia32_xabort(" << unsigned(E->AbortCode)
         << ");\n";
    } else if (Cpuid || Pause) {
      // Intel SDM: CPUID and PAUSE always abort RTM. Execute the actual
      // instruction; never invent an abort reason or use these for other
      // instructions whose abort behavior depends on the implementation.
      if (Cpuid)
        OS << R"C(#if __has_builtin(__builtin_ia32_cpuidex)
            int result[4];
            __builtin_ia32_cpuidex(result,(unsigned)state->GPR[0],(unsigned)state->GPR[1]);
#else
            uint32_t a,b,c,d;
            __asm__ volatile("cpuid" : "=a"(a),"=b"(b),"=c"(c),"=d"(d)
                             : "0"((uint32_t)state->GPR[0]),
                               "2"((uint32_t)state->GPR[1]) : "cc");
#endif
)C";
      else
        OS << "            __builtin_ia32_pause();\n";
      OS << "            " << Abort << "\n";
    } else {
      OS << "            uint64_t exit=" << Unit.Function.Name
         << "((uint8_t *)state);\n"
         << "            if (exit>" << unsigned(Exit::Return);
      if (!FollowInternalCalls)
        OS << " || exit==" << unsigned(Exit::Call)
           << " || exit==" << unsigned(Exit::Return);
      OS << ") { " << Abort << " }\n";
      Body.push_back(Hex(Unit.Address));
    }
    if (Begin || End || Test || ExplicitAbort || Cpuid || Pause)
      Controls.push_back(Hex(Unit.Address));
    OS << "            break;\n        }\n";
  }
  OS << "        default: " << Abort << "\n        }\n    }\n}\n";
  return llvm::json::Object{
      {"provider", "neverd_x64_rtm_environment_v6"},
      {"context", "neverd_x64_rtm_context_v6"},
      {"begins", std::move(Begins)},
      {"body_steps", std::move(Body)},
      {"native_controls", std::move(Controls)},
      {"standalone_controls", std::move(Standalone)},
      {"internal_abort_code", Refusal},
      {"domain", "CPUID RTM; entry outside RTM/HLE; successful ordinary RAM "
                 "disjoint from state, context, code and compiler frame; "
                 "no faults, debug traps or asynchronous events; immutable "
                 "original bytes; one outer native checkpoint per attempt; "
                 "nested aborts use the outer fallback; unsupported paths "
                 "and budget exhaustion roll back and decline; no progress "
                 "or native abort-reason equivalence is promised"}};
}

} // namespace neverd
