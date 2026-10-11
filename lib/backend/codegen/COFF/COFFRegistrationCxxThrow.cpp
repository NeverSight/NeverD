//===- COFFRegistrationCxxThrow.cpp - Direct PE32 throw closure -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Rebind original ThrowInfo and scalar storage before checking edited LLVM IR.
//===----------------------------------------------------------------------===//
#include "COFFRegistrationCxxIRProof.h"
#include "COFFRegistrationFrameProof.h"

#include "neverd/ir/low/RegistrationABI.h"

namespace neverd::coff_registration {
llvm::Error bindCxxRuntimeThrow(const CxxIRControlProof &Proof,
                                const llvm::CallBase &Call,
                                const CxxIRCall &Checked,
                                const BinaryImage &Image,
                                RegistrationCxxFrameContract &Contract,
                                std::vector<ExceptionAddressRange> &Immutable,
                                size_t &Work) {
  const auto Import = getCheckedX86RegistrationThrowImportABI(
      Image, Checked.Contract.Target, &Work);
  if (!Import || !Checked.RuntimeThrow)
    return rejectIR("C++ direct throw lost its original runtime ABI");
  Immutable.push_back({Import->IATVA, Import->IATVA + 4});
  const auto &Throw = *Checked.RuntimeThrow;
  if (Throw.isRethrow())
    return llvm::Error::success();
  const auto Info =
      coff_loader::getCheckedX86SimpleCxxThrowInfo(Image, Throw.ThrowInfoVA);
  if (!Info || Info->ObjectSize != Throw.ObjectSize)
    return rejectIR("C++ direct throw lost its checked scalar type");
  Immutable.insert(Immutable.end(), Info->ReadOnlyRanges.begin(),
                   Info->ReadOnlyRanges.end());
  Immutable.push_back(Info->TypeDescriptorRange);
  RegistrationFrameBorrow Borrow;
  Borrow.Offset = int64_t(Proof.Frame.Establisher) + Throw.ObjectOffset;
  Borrow.Reads.push_back({0, int32_t(Throw.ObjectSize)});
  Borrow.ThrowInfoVA = Throw.ThrowInfoVA;
  if (Throw.CallbackVA) {
    for (const auto &[Identity, Catch] : Proof.Catches)
      if (Proof.Source.ExceptionMetadata->Cxx->TryBlocks[Identity.first]
              .Handlers[Identity.second]
              .HandlerVA == Throw.CallbackVA) {
        if (Borrow.Root || !Catch.Stack)
          return rejectIR("direct throw has no unique callback stack");
        Borrow.Root = Catch.Stack;
      }
    const auto Bytes =
        Borrow.Root
            ? Borrow.Root->getAllocationSize(Call.getModule()->getDataLayout())
            : std::nullopt;
    if (!Bytes || Bytes->isScalable())
      return rejectIR("direct throw lost its callback object extent");
    Borrow.Offset = int64_t(Bytes->getFixedValue()) + Throw.ObjectOffset;
  }
  if (!Contract.Borrows.emplace(&Call, std::move(Borrow)).second)
    return rejectIR("direct throw object borrow was duplicated");
  return llvm::Error::success();
}
} // namespace neverd::coff_registration
