#ifndef NEVERD_SDK_CAPI_SOURCEREGISTERCOPYPROJECTION_H
#define NEVERD_SDK_CAPI_SOURCEREGISTERCOPYPROJECTION_H

#include "neverd/loader/MachO/SourceRegisterCopy.h"
#include "neverd/loader/ObjC/ObjCClassGetterCalls.h"
#include "neverd/pipeline/Pipeline.h"

namespace neverd::sdk {
class SourceRegisterCopyProjectionValidator {
public:
  SourceRegisterCopyProjectionValidator(const BinaryImage &Image,
                                        const PipelineResult &Result)
      : Image(Image), SameImage(Result.SourceImage == &Image) {
    for (const auto &Function : Result.MedFuncs)
      if (!Function.RegisterCopyProjections.empty() ||
          !Function.ClassGetterCallFacts.empty())
        Projected.emplace(Function.Entry, Pair{});
    for (const auto &Function : Result.MedFuncs)
      if (auto Found = Projected.find(Function.Entry);
          Found != Projected.end()) {
        auto &Pair = Found->second;
        Pair.Ambiguous |= Pair.Med != nullptr;
        Pair.Med = &Function;
      }
    for (const auto &Function : Result.LowFuncs)
      if (auto Found = Projected.find(Function.Entry);
          Found != Projected.end()) {
        auto &Pair = Found->second;
        Pair.Ambiguous |= Pair.Low != nullptr;
        Pair.Low = &Function;
      }
  }

  bool valid(const HighFunc &Function) const {
    if (!SameImage)
      return false;
    const auto Found = Projected.find(Function.Entry);
    if (Found == Projected.end())
      return Function.RegisterCopyProjections.empty() &&
             Function.ClassGetterCallFacts.empty();
    const auto &Pair = Found->second;
    if (Pair.Ambiguous || !Pair.Low || !Pair.Med ||
        Function.RegisterCopyProjections != Pair.Med->RegisterCopyProjections ||
        Function.ClassGetterCallFacts != Pair.Med->ClassGetterCallFacts ||
        (!Function.ClassGetterCallFacts.empty() &&
         !validateSourceClassGetterCalls(Image, *Pair.Low,
                                         Function.ClassGetterCallFacts)) ||
        !validateSourceRegisterCopies(Image, *Pair.Low,
                                      Function.RegisterCopyProjections))
      return false;
    std::map<SourceCallOccurrenceKey, unsigned> GetterCalls;
    for (const auto &[Site, Proof] : Function.ClassGetterCallFacts)
      GetterCalls.emplace(Site, 0);
    for (const auto &Block : Pair.Med->Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
          for (const auto &[Site, Copy] : Function.RegisterCopyProjections)
            if (Site.Instruction == Op.Addr && Site.Sequence == Op.OriginSeq)
              return false;
          const SourceCallOccurrenceKey Site{
              Op.Addr, Op.OriginSeq, Op.Opcode,
              Op.NumInputs && Op.Inputs[0].isConst()
                  ? std::optional<va_t>(Op.Inputs[0].ConstVal)
                  : std::nullopt};
          if (auto Found = GetterCalls.find(Site); Found != GetterCalls.end())
            ++Found->second;
        }
    for (const auto &[Site, Count] : GetterCalls)
      if (Count != 1)
        return false;
    return true;
  }

private:
  struct Pair {
    const LowFunc *Low = nullptr;
    const MedFunc *Med = nullptr;
    bool Ambiguous = false;
  };
  const BinaryImage &Image;
  bool SameImage;
  std::map<va_t, Pair> Projected;
};

inline bool sourceRegisterCopyProjectionValid(const HighFunc &Function,
                                              const BinaryImage &Image,
                                              const PipelineResult &Result) {
  return SourceRegisterCopyProjectionValidator(Image, Result).valid(Function);
}
} // namespace neverd::sdk
#endif
