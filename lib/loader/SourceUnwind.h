#ifndef NEVERD_LOADER_SOURCEUNWIND_H
#define NEVERD_LOADER_SOURCEUNWIND_H

#include "neverd/loader/ExceptionInfo.h"

namespace neverd {
/// Structural unwind metadata that introduces no language dispatch.
inline bool isPlainSourceUnwind(const ExceptionFunction &Metadata) {
  // Darwin emits unwind ranges for ordinary leaf methods too. Only a fully
  // decoded structural frame with no language-dispatch state is benign here.
  if (Metadata.ParseStatus != ExceptionParseStatus::Complete ||
      Metadata.Personality != ExceptionPersonality::None ||
      Metadata.PersonalityVA || !Metadata.PersonalityName.empty() ||
      Metadata.HandlerDataVA || Metadata.hasLanguageTable() ||
      Metadata.GSCookie || Metadata.ARMEHABI || Metadata.Rust)
    return false;
  if (Metadata.ObjC) {
    const auto &ObjC = *Metadata.ObjC;
    // A runtime synchronization call can occur in an ordinary C body without
    // a landing pad or personality. Its call binding is checked separately;
    // the presence of a language annotation alone does not imply a handler.
    if (ObjC.Runtime != ObjCRuntimeKind::AppleNonFragile ||
        ObjC.UsesFragileSetjmp || ObjC.UsesMSVCTables ||
        !ObjC.LandingPads.empty() || ObjC.RuntimeCalls.empty())
      return false;
    for (const auto &Call : ObjC.RuntimeCalls)
      if (Call.Kind != ObjCRuntimeCallKind::SyncEnter &&
          Call.Kind != ObjCRuntimeCallKind::SyncExit &&
          Call.Kind != ObjCRuntimeCallKind::ARCCleanup)
        return false;
  }
  if (Metadata.Encoding == ExceptionEncoding::CompactUnwind) {
    if (!Metadata.Compact)
      return false;
  } else if (Metadata.Encoding == ExceptionEncoding::DwarfFDE) {
    if (!Metadata.Dwarf)
      return false;
  } else {
    return false;
  }
  if (Metadata.Compact &&
      (Metadata.Compact->PersonalityVA || Metadata.Compact->HasLSDA ||
       Metadata.Compact->LSDAVA ||
       Metadata.Compact->SemanticStatus !=
           CompactUnwindSemanticStatus::Complete))
    return false;
  return !Metadata.Dwarf || Metadata.Dwarf->LSDAVA == 0;
}
} // namespace neverd
#endif
