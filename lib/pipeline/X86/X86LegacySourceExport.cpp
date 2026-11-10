//===- X86LegacySourceExport.cpp - Protected source selection ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Emit mode-sensitive dispatch while retaining one instruction-state owner.
//===----------------------------------------------------------------------===//

#include "neverd/pipeline/X86LegacySourceExport.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Limits.h"
#include "neverd/support/SHA256.h"
#include "neverd/translate/X86/X86LegacySourceExecution.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace neverd {
namespace {

llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, Message);
}

struct Instruction {
  uint32_t Address;
  bool Code32;
  llvm::ArrayRef<uint8_t> Bytes;

  uint64_t key() const {
    return translate::x86LegacySourceKey(Address, Code32);
  }
};

llvm::Expected<std::vector<Instruction>> readRequest(const BinaryImage &Image,
                                                     llvm::StringRef Text) {
  if (Text.empty() ||
      Text.size() > limits::pipeline::kX86LegacySourceRequestBytes)
    return invalid("legacy source request exceeds its byte bound");
  auto JSON = llvm::json::parse(Text);
  if (!JSON)
    return JSON.takeError();
  const auto *Object = JSON->getAsObject();
  if (!Object)
    return invalid("legacy source request must be an object");
  for (const auto &[Key, Value] : *Object)
    if (Key != "profile" && Key != "instructions")
      return invalid("unknown legacy source option: " + llvm::StringRef(Key));
  if (Object->getString("profile") != "protected-legacy-v1")
    return invalid("legacy source requires protected-legacy-v1");
  const auto *Rows = Object->getArray("instructions");
  if (!Rows || Rows->empty() ||
      Rows->size() > limits::pipeline::kX86LegacySourceInstructions)
    return invalid("legacy source requires a bounded instruction array");
  std::vector<Instruction> Result;
  Result.reserve(Rows->size());
  llvm::DenseSet<uint64_t> Seen;
  Seen.reserve(Rows->size());
  for (const auto &Row : *Rows) {
    const auto *Entry = Row.getAsObject();
    if (!Entry)
      return invalid("legacy source instruction must be an object");
    for (const auto &[Key, Value] : *Entry)
      if (Key != "address" && Key != "code_bits" && Key != "size")
        return invalid("unknown legacy source instruction field: " +
                       llvm::StringRef(Key));
    uint64_t Address = 0;
    if (const auto Number = Entry->getInteger("address");
        Number && *Number >= 0)
      Address = *Number;
    else if (const auto Text = Entry->getString("address");
             !Text || Text->empty() || Text->getAsInteger(0, Address))
      return invalid("legacy source instruction requires a uint32 address");
    const auto Bits = Entry->getInteger("code_bits");
    const auto Size = Entry->getInteger("size");
    if ((Bits != 16 && Bits != 32) || !Size || *Size < 1 || *Size > 15 ||
        Address > UINT32_MAX || uint64_t(*Size - 1) > UINT32_MAX - Address)
      return invalid("legacy source requires code_bits 16/32 and an exact "
                     "1..15 byte extent within the 32-bit image");
    const auto *Bytes = Image.readVA(Address, *Size);
    if (!Bytes)
      return invalid("legacy source instruction lacks complete image bytes");
    Instruction I{uint32_t(Address), Bits == 32,
                  llvm::ArrayRef(Bytes, size_t(*Size))};
    if (auto Error =
            translate::validateX86LegacyInstructionEncoding(I.Bytes, I.Code32))
      return std::move(Error);
    if (!Seen.insert(I.key()).second)
      return invalid("duplicate legacy source address and code mode");
    Result.push_back(I);
  }
  std::sort(Result.begin(), Result.end(),
            [](const auto &A, const auto &B) { return A.key() < B.key(); });
  return Result;
}

} // namespace

llvm::Expected<X86LegacySourceExport>
exportX86LegacySource(const BinaryImage &Image, llvm::StringRef RequestJSON) {
  if (Image.Arch != Arch::X86)
    return invalid("protected legacy source requires an i386 image");
  auto Units = readRequest(Image, RequestJSON);
  if (!Units)
    return Units.takeError();
  using Status = translate::X86LegacyExecutionStatus;
  using Exit = translate::X86LegacySourceExit;
  X86LegacySourceExport Result;
  llvm::raw_string_ostream OS(Result.Source);
  OS << R"C(/* Protected legacy original-byte dispatch. Link the matching NeverD runtime.
 * Context borrows X86LegacySourceExecution with complete state and profiles.
 * Calls/returns follow selected targets. Events stop before handler execution.
 * Retain all state and RAM on every stop; resume a pending event before retry.
 * Budget is separate host storage, disjoint from state/RAM and callback writes.
 * This is runtime-backed C, not an inferred ordinary function ABI. */
#include <stdint.h>
extern uint32_t neverd_x86_legacy_select_v1(void *, uint64_t *);
extern uint32_t neverd_x86_legacy_step_v1(void *, uint64_t,
                                        const uint8_t *, uint32_t);
extern uint32_t neverd_x86_legacy_attempted_v1(void *);
)C";
  llvm::json::Object Exits;
  const auto EmitExit = [&](llvm::StringRef Name, auto Value) {
    OS << "#define NEVERD_X86_LEGACY_" << Name << " " << uint32_t(Value)
       << "u\n";
    Exits[Name] = uint32_t(Value);
  };
  EmitExit("COMPLETED", Status::Completed);
  EmitExit("EVENT_DELIVERED", Status::EventDelivered);
  EmitExit("EVENT_DEFERRED", Status::EventDeferred);
  EmitExit("CODE_CHANGED", Status::CodeChanged);
  EmitExit("BACKING_FAULT", Status::BackingFault);
  EmitExit("SHUTDOWN", Status::Shutdown);
  EmitExit("REPEAT_CONTINUE", Status::RepeatContinue);
  EmitExit("DEVICE_UNAVAILABLE", Status::DeviceUnavailable);
  EmitExit("EVENT_BUDGET_EXHAUSTED", Status::EventBudgetExhausted);
  EmitExit("HALTED", Status::Halted);
  EmitExit("INVALID_STATE", Exit::InvalidState);
  EmitExit("UNSELECTED", Exit::Unselected);
  EmitExit("BUDGET_EXHAUSTED", Exit::BudgetExhausted);
  EmitExit("PENDING_EVENT", Exit::PendingEvent);
  OS << R"C(
uint32_t neverd_x86_legacy_continue_v1(void *context, uint64_t *budget) {
    if (!context || !budget) return NEVERD_X86_LEGACY_INVALID_STATE;
    while (*budget) {
        uint64_t key;
        uint32_t status = neverd_x86_legacy_select_v1(context, &key);
        if (status != NEVERD_X86_LEGACY_COMPLETED) return status;
        switch (key) {
)C";
  llvm::json::Array Entries;
  for (const auto &I : *Units) {
    OS << "        case 0x" << llvm::utohexstr(I.key())
       << "ULL: {\n"
          "            static const uint8_t bytes[] = {";
    for (auto Byte : I.Bytes)
      OS << unsigned(Byte) << ',';
    OS << "};\n            status = neverd_x86_legacy_step_v1(\n"
          "                context, key, bytes, sizeof(bytes));\n"
          "            break;\n        }\n";
    Entries.push_back(
        llvm::json::Object{{"address", "0x" + llvm::utohexstr(I.Address)},
                           {"code_bits", I.Code32 ? 32 : 16},
                           {"size", I.Bytes.size()},
                           {"bytes", llvm::toHex(I.Bytes, true)}});
    if (Result.Source.size() > limits::pipeline::kX86LegacySourceBytes)
      return invalid("legacy source exceeds its output byte bound");
  }
  OS << R"C(        default: return NEVERD_X86_LEGACY_UNSELECTED;
        }
        if (neverd_x86_legacy_attempted_v1(context)) --*budget;
        if (status != NEVERD_X86_LEGACY_COMPLETED &&
            status != NEVERD_X86_LEGACY_REPEAT_CONTINUE) return status;
    }
    return NEVERD_X86_LEGACY_BUDGET_EXHAUSTED;
}
)C";
  if (Result.Source.size() > limits::pipeline::kX86LegacySourceBytes)
    return invalid("legacy source exceeds its output byte bound");
  llvm::raw_string_ostream Manifest(Result.Manifest);
  Manifest << llvm::formatv(
      "{0:2}\n",
      llvm::json::Value(llvm::json::Object{
          {"profile", "protected-legacy-v1"},
          {"source_route", "original-byte-runtime"},
          {"runtime", "X86LegacySourceExecution"},
          {"driver", "neverd_x86_legacy_continue_v1"},
          {"selection", "uint32(CS.base + EIP) and CS.D"},
          {"instruction_budget", "attempts; one per REP element"},
          {"event_policy", "stop; retain delivery before resume"},
          {"halt_policy", "HLT commits once; wait without fetch; eligible "
                          "IRQ/NMI/debug trap wakes before IDT access"},
          {"state_abi", "private C++ embedding; no serialized layout"},
          {"source_sha256",
           llvm::toHex(sha256(llvm::arrayRefFromStringRef(Result.Source)),
                       true)},
          {"exits", std::move(Exits)},
          {"instructions", std::move(Entries)}}));
  return Result;
}

} // namespace neverd
