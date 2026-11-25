//===- Arm64eWritebackModel.c - Compare CPA2 post-index results ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "Arm64eCheckedWriteback.def"

int compareStructureWriteback(int Enabled) {
  const WritebackStatus Expected =
      Enabled ? WritebackChecked : WritebackUnchecked;
  return observeStructureWriteback() != Expected;
}
