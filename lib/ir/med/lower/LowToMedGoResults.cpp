//===- LowToMedGoResults.cpp - Go register ABI call results ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Models the results a call under Go's internal register ABI returns past
/// its first one.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/CallRegisterContract.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/GoCallContract.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedGoResults.h"
#include "neverd/loader/BinaryImage.h"

#include <map>
#include <set>

namespace neverd {

void LowToMedConverter::modelGoCallResults(MedFunc &Func) {
  // Go's internal ABI (src/cmd/compile/abi-internal.md) returns a call's
  // integer results in RAX, RBX, RCX, RDI, RSI and R8-R11, in that order, and
  // keeps none of those registers but its results ("There are no callee-save
  // registers"): a read of one after the call, before any write, is a result,
  // and so is every register before it.  An indirect call compiled Go code
  // makes enters Go code by the internal ABI, as a function value always
  // does, and stands alone; a direct call shares its callee's shape with the
  // callee's other calls (materializeGoCallResults).
  if ((TargetArch != Arch::X64 && TargetArch != Arch::AArch64) || !Image ||
      !Image->ExceptionMetadata.GoModule ||
      !Image->ExceptionMetadata.GoModule->RegisterArguments ||
      !goCallEntersInternalABI(*Image, Func.Entry, InvalidVA, nullptr))
    return;
  std::set<uint32_t> Sites;
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::INDIR_CALL &&
          Op.RegisterContract == CallRegisterContract::GoInternal &&
          !Op.SourceCallHint && !Op.DoesNotReturn && !Op.RegisterReturnFields &&
          Op.Output.Kind == MedVar::Reg &&
          Op.Output.RegOff == goIntegerArgumentRegisters(TargetArch)[0])
        Sites.insert(Op.CallSiteId);
  // An indirect call's results are words: no callee shares its record, and
  // the record the pipeline extends (materializeGoCallResults) keeps them.
  std::map<uint32_t, GoCallShape> Shapes =
      goCallResultsRead(Func, TargetArch, Sites);
  const uint16_t Word = getTargetRegInfo(TargetArch).PointerSize;
  for (auto &[Site, Shape] : Shapes)
    for (uint16_t &Width : Shape.Widths)
      Width = Word;
  defineGoCallResults(Func, TargetArch, Shapes);
}

} // namespace neverd
