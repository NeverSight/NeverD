//===- StreamInternal.h - Private transcript state ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Temporary framing and recorded-identity material, discarded before return.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Streams.h"

namespace neverd::web::stream {

struct RecordedIdentity {
  std::string Session, ID;
  bool Request = false, Response = false;
};
struct Reader {
  StreamCapture Capture;
  std::vector<RecordedIdentity> Identities;
  uint64_t PrivateBytes = 0;
  bool CoverageGap = false;

  void add(StreamRecord Record, RecordedIdentity Identity = {});
  void record(std::string_view Bytes, uint64_t Offset, uint64_t Length,
              uint64_t Line, bool Terminated);
  void relate();
};

} // namespace neverd::web::stream
