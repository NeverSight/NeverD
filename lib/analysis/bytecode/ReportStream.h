//===- ReportStream.h - Size-limited report serialization ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Buffer LLVM JSON writes without growing an export past the frontend limit.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_BYTECODE_REPORTSTREAM_H
#define NEVERD_ANALYSIS_BYTECODE_REPORTSTREAM_H

#include "llvm/Support/raw_ostream.h"

#include <string>

namespace neverd::bytecode {
class ReportStream final : public llvm::raw_ostream {
  std::string &Text;
  size_t Limit;
  bool Full = false;

  void write_impl(const char *Data, size_t Size) override {
    if (Full)
      return;
    if (Size > Limit - Text.size()) {
      Full = true;
      return;
    }
    Text.append(Data, Size);
  }
  uint64_t current_pos() const override { return Text.size(); }

public:
  ReportStream(std::string &Text, size_t Limit) : Text(Text), Limit(Limit) {}
  bool exceeded() const { return Full; }
};
} // namespace neverd::bytecode

#endif // NEVERD_ANALYSIS_BYTECODE_REPORTSTREAM_H
