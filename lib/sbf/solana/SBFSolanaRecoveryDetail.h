//===- SBFSolanaRecoveryDetail.h - Private recovery state -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The memory view and the recovery pass state shared by the Solana fact
/// recovery translation units: the driver (SBFSolanaRecovery.cpp) and the
/// per-instruction visitors (SBFSolanaRecoveryVisitors.cpp).
///
/// This header is an implementation detail of the sbf/solana library and
/// should NOT be included by code outside lib/sbf/solana/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_SOLANA_SBFSOLANARECOVERYDETAIL_H
#define NEVERD_SBF_SOLANA_SBFSOLANARECOVERYDETAIL_H

#include "neverd/sbf/SBFIR.h"
#include "neverd/sbf/analysis/SBFDataflow.h"
#include "neverd/sbf/solana/SBFCPI.h"
#include "neverd/sbf/solana/SBFSolanaRecovery.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Compiler.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::sbf {
namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE solana_recovery_detail {

/// A constant register value that provably addresses \c kPubkeyByteCount
/// readable bytes.
struct AddressedKey {
  va_t Address;
  Pubkey Key;
};

/// Ordinals of the comparison signature recovery reads:
///
///   sol_memcmp_(left, right, length, result)
enum class CompareArgument : unsigned { Left = 0, Right, Length, Result };

constexpr unsigned argumentRegister(CompareArgument Argument) {
  return kFirstArgumentRegister + static_cast<unsigned>(Argument);
}

/// Everything a program point has proven about memory: the bytes the image
/// maps, plus the bytes the program wrote into its own frame and heap.
///
/// A program assembles the argument of an invocation or a derivation in its
/// own scratch memory, so reading only the image would see the pointer and
/// none of what it points at.
class BlockMemory final : public MemorySource {
public:
  BlockMemory(const ProgramImage &Image, const MemoryModel &Scratch)
      : Mapped(Image), Scratch(Scratch) {}

  std::optional<uint64_t> readWord(va_t Address) const override {
    if (const std::optional<uint64_t> Written = Scratch.readWord(Address))
      return Written;
    return Mapped.readWord(Address);
  }

  llvm::ArrayRef<uint8_t> readBytes(va_t Address, size_t Size) const override {
    if (const llvm::ArrayRef<uint8_t> Written = Scratch.read(Address, Size);
        !Written.empty())
      return Written;
    return Mapped.readBytes(Address, Size);
  }

private:
  ImageMemorySource Mapped;
  const MemoryModel &Scratch;
};

class Recovery {
public:
  Recovery(const SBFProgram &Program, const SolanaRecoveryOptions &Options)
      : Program(Program), Options(Options), Index(Program.Med) {
    // CPI/PDA visitors are the only consumers that dereference program-built
    // scratch descriptors. Programs without such a call must not pay for a
    // whole-CFG memory fixed point.
    if (std::any_of(Program.Med.Instructions.begin(),
                    Program.Med.Instructions.end(), consumesScratchFacts))
      Flow.emplace(Program, Index);
    if (Flow &&
        Flow->statistics().Precision == ScratchFlowPrecision::WidenedToUnknown)
      Model.ScratchPrecision = ScratchRecoveryPrecision::BlockLocal;
  }

  SolanaModel run();

private:
  /// Read a key from \p Address when the image maps that many data bytes.
  std::optional<AddressedKey> readKeyAt(va_t Address) const;

  /// The constant a register holds, if it holds one.
  static std::optional<uint64_t> constantOf(const RegisterValue &Value);

  /// The address a register designates, whether by constant or by frame
  /// offset.
  static std::optional<va_t> addressOf(const RegisterValue &Value);

  void noteKey(va_t Address, const Pubkey &Key, bool ReferencedByCode);

  void visitMemoryAccess(const MedInstruction &Instruction,
                         const MachineState &State);
  void visitComparison(const MedInstruction &Instruction,
                       const MachineState &State);
  void visitSyscall(const MedInstruction &Instruction,
                    const MachineState &State);
  void visitInvocation(const MedInstruction &Instruction,
                       const MachineState &State, const SyscallInfo &Info);
  void visitDerivation(const MedInstruction &Instruction,
                       const MachineState &State, const SyscallInfo &Info);

  void visitExit(const MedInstruction &Instruction, const MachineState &State);

  /// Name the operation the payload of \p Site selects.
  void nameInvokedOperation(CPISite &Site, llvm::ArrayRef<uint8_t> Payload);

  /// Attach a name to a discriminator, from the supplied IDL first and the
  /// built-in dictionary second. Returns false when neither knows it.
  bool nameDiscriminator(const AnchorDiscriminator &Value, std::string &Name,
                         AnchorNamespace &Namespace,
                         std::optional<RecoveryEvidence> &Evidence) const;

  void scanReadOnlyData();
  void resolveHandlers();
  void resolveErrors();
  void addLints();

  const SBFProgram &Program;
  const SolanaRecoveryOptions &Options;
  MedInstructionIndex Index;
  std::optional<ScratchFlow> Flow;
  ScratchState EmptyScratch;
  SolanaModel Model;

  /// Discriminator candidates in program order, before name resolution.
  struct Candidate {
    uint64_t Word;
    size_t CompareSlot;
    std::optional<size_t> TargetSlot;
  };
  std::vector<Candidate> Candidates;

  /// Constants that reach a return, before they are judged to be error codes.
  std::vector<ReturnedError> ErrorCandidates;

  /// Keys already recorded, bucketed by a cheap prefix before full comparison.
  /// A hostile binary controls key bytes, so a prefix collision must neither
  /// duplicate a key nor merge two distinct keys.
  llvm::DenseMap<uint64_t, llvm::SmallVector<size_t, 1>> KeyIndicesByFirstWord;
};

} // namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE solana_recovery_detail
} // namespace neverd::sbf

#endif // NEVERD_SBF_SOLANA_SBFSOLANARECOVERYDETAIL_H
