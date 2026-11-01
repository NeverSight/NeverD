//===- X64RtmAbortRegions.cpp - Bound immediate RTM aborts ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep the complete native transaction in one assembly block and transport
/// its actual abort status without guessing an outcome or rolling back C.
//===----------------------------------------------------------------------===//

#include "X64RtmAbortRegions.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/decode/X86/X86TransactionEncoding.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace neverd {

std::optional<X64RtmAbortRegion> readX64RtmAbortRegion(const BinaryImage &Image,
                                                       va_t Address,
                                                       unsigned AddressBits) {
  if (Image.Arch != Arch::X64 || (AddressBits != 48 && AddressBits != 57) ||
      Address > std::numeric_limits<va_t>::max() - 8)
    return std::nullopt;
  const auto *Bytes = Image.readVA(Address, 9);
  if (!Bytes || Bytes[0] != 0xc7 || Bytes[1] != 0xf8 || Bytes[6] != 0xc6 ||
      Bytes[7] != 0xf8 || !Image.isCodeAddress(Address) ||
      !Image.isCodeAddress(Address + 8))
    return std::nullopt;
  const auto Begin = x86TransactionBeginEncoding({Bytes, 6}, Address, true);
  if (!Begin)
    return std::nullopt;
  const uint64_t Sign = uint64_t(1) << (AddressBits - 1);
  const uint64_t Canonical =
      (Begin->Target & (Sign - 1)) | ((Begin->Target & Sign) ? ~(Sign - 1) : 0);
  if (Begin->Target != Canonical)
    return std::nullopt;
  X64RtmAbortRegion Region{Address, Begin->Target, {}};
  std::copy_n(Bytes, Region.Bytes.size(), Region.Bytes.begin());
  return Region;
}

llvm::json::Array emitX64RtmAbortEnvironment(
    llvm::raw_ostream &OS, const BinaryImage &Image,
    llvm::ArrayRef<analysis::X64ContinuationInstructionSource> Units,
    unsigned AddressBits) {
  std::vector<X64RtmAbortRegion> Regions;
  for (const auto &Unit : Units)
    if (Unit.RequiresNativeExecution && Unit.Bytes.size() == 6 &&
        Unit.Bytes[0] == 0xc7 && Unit.Bytes[1] == 0xf8)
      if (auto Region = readX64RtmAbortRegion(Image, Unit.Address, AddressBits))
        Regions.push_back(*Region);
  llvm::json::Array Receipts;
  OS << R"C(
/* Explicit immediate-abort RTM environment, for use with neverd_x64_run_v6.
 * Invoke outside RTM/HLE in the declared nonfaulting, no-debug-trap domain.
 * The complete pair runs inside one assembly block; no transaction spans a
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
    const auto Fallback = "0x" + llvm::utohexstr(Region.Fallback);
    OS << "    case " << PC
       << "ULL: {\n"
          "        uint32_t a,b,c,d,status;\n"
          "        __asm__ volatile(\"cpuid\" : "
          "\"=a\"(a),\"=b\"(b),\"=c\"(c),\"=d\"(d)\n"
          "                         : \"0\"(0),\"2\"(0) : \"cc\");\n"
          "        if (a < 7) return 0;\n"
          "        __asm__ volatile(\"cpuid\" : "
          "\"=a\"(a),\"=b\"(b),\"=c\"(c),\"=d\"(d)\n"
          "                         : \"0\"(7),\"2\"(0) : \"cc\");\n"
          "        if (!(b & (1U << 11))) return 0;\n"
          "        __asm__ volatile(\"xbegin 1f; xabort $"
       << unsigned(Region.Bytes[8])
       << "; 1:\" : \"=a\"(status) : : \"memory\",\"cc\");\n"
          "        state->GPR[0]=status;\n"
          "        state->PC="
       << Fallback << "ULL;\n        return 1;\n    }\n";
    Receipts.push_back(
        llvm::json::Object{{"pc", PC},
                           {"bytes", llvm::toHex(Region.Bytes)},
                           {"fallback", Fallback},
                           {"abort_code", unsigned(Region.Bytes[8])}});
  }
  OS << "    default: return 0;\n    }\n}\n";
  return Receipts;
}

} // namespace neverd
