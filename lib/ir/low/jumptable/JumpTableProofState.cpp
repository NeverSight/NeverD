//===- JumpTableProofState.cpp - Complete table proof state
//----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/CFGBuilder.h"

namespace neverd {

std::optional<bool> CFGBuilder::compareJumpTableInfo(const JumpTableInfo &Left,
                                                     const JumpTableInfo &Right,
                                                     size_t &Remaining) const {
  auto ConsumeProposalStageEvidence = [&](size_t Amount = 1) {
    if (Amount > Remaining) {
      Remaining = 0;
      return false;
    }
    Remaining -= Amount;
    return true;
  };
  auto SameNdVarNoShortCircuit =
      [&](const NdVar *Left, const NdVar *Right) -> std::optional<bool> {
    if (!ConsumeProposalStageEvidence(5))
      return std::nullopt;
    if (!Left || !Right)
      return false;
    bool Same = true;
    Same &= Left->Space == Right->Space;
    Same &= Left->Offset == Right->Offset;
    Same &= Left->Size == Right->Size;
    Same &= Left->Provenance == Right->Provenance;
    Same &= Left->AddressOwnerVA == Right->AddressOwnerVA;
    return Same;
  };
  auto SameOccurrenceNoShortCircuit =
      [&](const JumpTableValueOccurrence *Left,
          const JumpTableValueOccurrence *Right) -> std::optional<bool> {
    if (!ConsumeProposalStageEvidence(3))
      return std::nullopt;
    const std::optional<bool> SameValue = SameNdVarNoShortCircuit(
        Left ? &Left->Value : nullptr, Right ? &Right->Value : nullptr);
    if (!SameValue)
      return std::nullopt;
    if (!Left || !Right)
      return false;
    bool Same = *SameValue;
    Same &= Left->Addr == Right->Addr;
    Same &= Left->Seq == Right->Seq;
    Same &= Left->DefinedAtPoint == Right->DefinedAtPoint;
    return Same;
  };
  auto SameStorageRangeNoShortCircuit =
      [&](const JumpTableStorageRange *Left,
          const JumpTableStorageRange *Right) -> std::optional<bool> {
    if (!ConsumeProposalStageEvidence(4))
      return std::nullopt;
    if (!Left || !Right)
      return false;
    bool Same = true;
    Same &= Left->BaseAddr == Right->BaseAddr;
    Same &= Left->EntrySize == Right->EntrySize;
    Same &= Left->EntryStride == Right->EntryStride;
    Same &= Left->PhysicalSlotCount == Right->PhysicalSlotCount;
    return Same;
  };
  auto SameMaskWitnessNoShortCircuit =
      [&](const JumpTableMaskKnownOneWitness *Left,
          const JumpTableMaskKnownOneWitness *Right) -> std::optional<bool> {
    if (!ConsumeProposalStageEvidence(1))
      return std::nullopt;
    const std::optional<bool> SameOr = SameOccurrenceNoShortCircuit(
        Left ? &Left->OrOutput : nullptr, Right ? &Right->OrOutput : nullptr);
    if (!SameOr)
      return std::nullopt;
    const std::optional<bool> SameMask =
        SameOccurrenceNoShortCircuit(Left ? &Left->MaskOutput : nullptr,
                                     Right ? &Right->MaskOutput : nullptr);
    if (!SameMask)
      return std::nullopt;
    const std::optional<bool> SameConstant =
        SameNdVarNoShortCircuit(Left ? &Left->ConstantOperand : nullptr,
                                Right ? &Right->ConstantOperand : nullptr);
    if (!SameConstant)
      return std::nullopt;
    if (!Left || !Right)
      return false;
    bool Same = *SameOr & *SameMask & *SameConstant;
    Same &= Left->KnownOneBits == Right->KnownOneBits;
    return Same;
  };
  auto SameFrameAddressUseNoShortCircuit =
      [&](const JumpTableFrameAddressUse *Left,
          const JumpTableFrameAddressUse *Right) -> std::optional<bool> {
    if (!ConsumeProposalStageEvidence(1))
      return std::nullopt;
    const std::optional<bool> SameUse = SameOccurrenceNoShortCircuit(
        Left ? &Left->Use : nullptr, Right ? &Right->Use : nullptr);
    if (!SameUse)
      return std::nullopt;
    if (!Left || !Right)
      return false;
    bool Same = *SameUse;
    Same &= Left->ByteAddend == Right->ByteAddend;
    return Same;
  };
  auto SameStaticSourcePieceNoShortCircuit =
      [&](const JumpTableFrameInitializerChunk::StaticSourcePiece *Left,
          const JumpTableFrameInitializerChunk::StaticSourcePiece *Right)
      -> std::optional<bool> {
    if (!ConsumeProposalStageEvidence(6))
      return std::nullopt;
    const std::optional<bool> SameValue = SameOccurrenceNoShortCircuit(
        Left ? &Left->Value : nullptr, Right ? &Right->Value : nullptr);
    if (!SameValue)
      return std::nullopt;
    const std::optional<bool> SameAddress = SameOccurrenceNoShortCircuit(
        Left ? &Left->Address : nullptr, Right ? &Right->Address : nullptr);
    if (!SameAddress)
      return std::nullopt;
    const std::optional<bool> SameProducer = SameOccurrenceNoShortCircuit(
        Left ? &Left->StaticAddressProducer : nullptr,
        Right ? &Right->StaticAddressProducer : nullptr);
    if (!SameProducer)
      return std::nullopt;
    if (!Left || !Right)
      return false;
    bool Same = *SameValue & *SameAddress & *SameProducer;
    Same &= Left->StaticAddressFieldVA == Right->StaticAddressFieldVA;
    Same &= Left->StaticAddressProducerTargetVA ==
            Right->StaticAddressProducerTargetVA;
    Same &= Left->StaticAddress == Right->StaticAddress;
    Same &= Left->ByteCount == Right->ByteCount;
    Same &= Left->StaticAddressProvenance == Right->StaticAddressProvenance;
    Same &= Left->StaticAddressOwnerVA == Right->StaticAddressOwnerVA;
    return Same;
  };
  auto SameInitializerNoShortCircuit =
      [&](const JumpTableFrameInitializerChunk *Left,
          const JumpTableFrameInitializerChunk *Right) -> std::optional<bool> {
    // Seven direct scalar fields plus the StaticSources vector size.
    if (!ConsumeProposalStageEvidence(8))
      return std::nullopt;
    bool Same = Left && Right;
    if (Left && Right) {
      Same &= Left->ByteCount == Right->ByteCount;
      Same &= Left->StaticSourceAddress == Right->StaticSourceAddress;
      Same &= Left->StaticSourceProvenance == Right->StaticSourceProvenance;
      Same &= Left->StaticSourceOwnerVA == Right->StaticSourceOwnerVA;
      Same &= Left->StaticSourceFieldVA == Right->StaticSourceFieldVA;
      Same &= Left->StaticSourceProducerTargetVA ==
              Right->StaticSourceProducerTargetVA;
      Same &= Left->IsMemcpy == Right->IsMemcpy;
      Same &= Left->StaticSources.size() == Right->StaticSources.size();
    }
    const std::optional<bool> SameWriter = SameOccurrenceNoShortCircuit(
        Left ? &Left->Writer : nullptr, Right ? &Right->Writer : nullptr);
    if (!SameWriter)
      return std::nullopt;
    Same &= *SameWriter;
    const std::optional<bool> SameDestination =
        SameFrameAddressUseNoShortCircuit(Left ? &Left->Destination : nullptr,
                                          Right ? &Right->Destination
                                                : nullptr);
    if (!SameDestination)
      return std::nullopt;
    Same &= *SameDestination;
    const std::optional<bool> SameStored =
        SameOccurrenceNoShortCircuit(Left ? &Left->StoredValue : nullptr,
                                     Right ? &Right->StoredValue : nullptr);
    if (!SameStored)
      return std::nullopt;
    Same &= *SameStored;
    const std::optional<bool> SameSource =
        SameOccurrenceNoShortCircuit(Left ? &Left->SourceAddress : nullptr,
                                     Right ? &Right->SourceAddress : nullptr);
    if (!SameSource)
      return std::nullopt;
    Same &= *SameSource;
    const std::optional<bool> SameLength = SameOccurrenceNoShortCircuit(
        Left ? &Left->Length : nullptr, Right ? &Right->Length : nullptr);
    if (!SameLength)
      return std::nullopt;
    Same &= *SameLength;
    const std::optional<bool> SameLengthProducer =
        SameOccurrenceNoShortCircuit(Left ? &Left->LengthProducer : nullptr,
                                     Right ? &Right->LengthProducer : nullptr);
    if (!SameLengthProducer)
      return std::nullopt;
    Same &= *SameLengthProducer;
    const std::optional<bool> SameStaticProducer = SameOccurrenceNoShortCircuit(
        Left ? &Left->StaticSourceProducer : nullptr,
        Right ? &Right->StaticSourceProducer : nullptr);
    if (!SameStaticProducer)
      return std::nullopt;
    Same &= *SameStaticProducer;
    const size_t LeftSourceCount = Left ? Left->StaticSources.size() : 0;
    const size_t RightSourceCount = Right ? Right->StaticSources.size() : 0;
    for (size_t I = 0; I < std::max(LeftSourceCount, RightSourceCount); ++I) {
      const auto *LeftSource =
          Left && I < LeftSourceCount ? &Left->StaticSources[I] : nullptr;
      const auto *RightSource =
          Right && I < RightSourceCount ? &Right->StaticSources[I] : nullptr;
      const std::optional<bool> SameSourcePiece =
          SameStaticSourcePieceNoShortCircuit(LeftSource, RightSource);
      if (!SameSourcePiece)
        return std::nullopt;
      Same &= *SameSourcePiece;
    }
    return Same;
  };
  auto SameFrameStorageNoShortCircuit =
      [&](const JumpTableFrameStorageRole *Left,
          const JumpTableFrameStorageRole *Right) -> std::optional<bool> {
    if (!ConsumeProposalStageEvidence(1))
      return std::nullopt;
    bool Same = Left && Right;
    if (Left && Right)
      Same &= Left->Initializers.size() == Right->Initializers.size();
    const std::optional<bool> SameRuntimeBase =
        SameFrameAddressUseNoShortCircuit(Left ? &Left->RuntimeBase : nullptr,
                                          Right ? &Right->RuntimeBase
                                                : nullptr);
    if (!SameRuntimeBase)
      return std::nullopt;
    Same &= *SameRuntimeBase;
    const std::optional<bool> SameCompleteAddress =
        SameOccurrenceNoShortCircuit(Left ? &Left->CompleteAddress : nullptr,
                                     Right ? &Right->CompleteAddress : nullptr);
    if (!SameCompleteAddress)
      return std::nullopt;
    Same &= *SameCompleteAddress;
    const size_t LeftInitializerCount = Left ? Left->Initializers.size() : 0;
    const size_t RightInitializerCount = Right ? Right->Initializers.size() : 0;
    for (size_t I = 0;
         I < std::max(LeftInitializerCount, RightInitializerCount); ++I) {
      const auto *LeftInitializer =
          Left && I < LeftInitializerCount ? &Left->Initializers[I] : nullptr;
      const auto *RightInitializer = Right && I < RightInitializerCount
                                         ? &Right->Initializers[I]
                                         : nullptr;
      const std::optional<bool> SameInitializer =
          SameInitializerNoShortCircuit(LeftInitializer, RightInitializer);
      if (!SameInitializer)
        return std::nullopt;
      Same &= *SameInitializer;
    }
    return Same;
  };
  auto SameDisplacedAddressNoShortCircuit =
      [&](const JumpTableDisplacedAddressRole *Left,
          const JumpTableDisplacedAddressRole *Right) -> std::optional<bool> {
    if (!ConsumeProposalStageEvidence(2))
      return std::nullopt;
    const std::optional<bool> SameRuntimeBase =
        SameOccurrenceNoShortCircuit(Left ? &Left->RuntimeBaseUse : nullptr,
                                     Right ? &Right->RuntimeBaseUse : nullptr);
    if (!SameRuntimeBase)
      return std::nullopt;
    const std::optional<bool> SameCompleteAddress =
        SameOccurrenceNoShortCircuit(Left ? &Left->CompleteAddress : nullptr,
                                     Right ? &Right->CompleteAddress : nullptr);
    if (!SameCompleteAddress)
      return std::nullopt;
    if (!Left || !Right)
      return false;
    bool Same = *SameRuntimeBase & *SameCompleteAddress;
    Same &= Left->ExpectedRuntimeBase == Right->ExpectedRuntimeBase;
    Same &= Left->ByteAddend == Right->ByteAddend;
    return Same;
  };
  auto SameScalarVectorNoShortCircuit =
      [&](const auto &Left, const auto &Right) -> std::optional<bool> {
    const std::optional<size_t> Work =
        detail::scalarVectorComparisonWork(Left.size(), Right.size());
    if (!Work || !ConsumeProposalStageEvidence(*Work))
      return std::nullopt;
    bool Same = Left.size() == Right.size();
    for (size_t I = 0; I < std::min(Left.size(), Right.size()); ++I)
      Same &= Left[I] == Right[I];
    return Same;
  };
  auto SameOccurrenceVectorNoShortCircuit =
      [&](const auto &Left, const auto &Right) -> std::optional<bool> {
    if (!ConsumeProposalStageEvidence(1))
      return std::nullopt;
    bool Same = Left.size() == Right.size();
    for (size_t I = 0; I < std::max(Left.size(), Right.size()); ++I) {
      const auto *LeftOccurrence = I < Left.size() ? &Left[I] : nullptr;
      const auto *RightOccurrence = I < Right.size() ? &Right[I] : nullptr;
      const std::optional<bool> SameOccurrence =
          SameOccurrenceNoShortCircuit(LeftOccurrence, RightOccurrence);
      if (!SameOccurrence)
        return std::nullopt;
      Same &= *SameOccurrence;
    }
    return Same;
  };
  auto SameStorageRangeVectorNoShortCircuit =
      [&](const auto &Left, const auto &Right) -> std::optional<bool> {
    if (!ConsumeProposalStageEvidence(1))
      return std::nullopt;
    bool Same = Left.size() == Right.size();
    for (size_t I = 0; I < std::max(Left.size(), Right.size()); ++I) {
      const auto *LeftRange = I < Left.size() ? &Left[I] : nullptr;
      const auto *RightRange = I < Right.size() ? &Right[I] : nullptr;
      const std::optional<bool> SameRange =
          SameStorageRangeNoShortCircuit(LeftRange, RightRange);
      if (!SameRange)
        return std::nullopt;
      Same &= *SameRange;
    }
    return Same;
  };
  auto SameWitnessVectorNoShortCircuit =
      [&](const auto &Left, const auto &Right) -> std::optional<bool> {
    if (!ConsumeProposalStageEvidence(1))
      return std::nullopt;
    bool Same = Left.size() == Right.size();
    for (size_t I = 0; I < std::max(Left.size(), Right.size()); ++I) {
      const auto *LeftWitness = I < Left.size() ? &Left[I] : nullptr;
      const auto *RightWitness = I < Right.size() ? &Right[I] : nullptr;
      const std::optional<bool> SameWitness =
          SameMaskWitnessNoShortCircuit(LeftWitness, RightWitness);
      if (!SameWitness)
        return std::nullopt;
      Same &= *SameWitness;
    }
    return Same;
  };
  auto SameLoadRoleNoShortCircuit =
      [&](const JumpTableLoadRole *Left,
          const JumpTableLoadRole *Right) -> std::optional<bool> {
    // Direct scalars excluding the two vector sizes and nested certificates.
    if (!ConsumeProposalStageEvidence(13))
      return std::nullopt;
    bool Same = Left && Right;
    if (Left && Right) {
      Same &= Left->LoadWidth == Right->LoadWidth;
      Same &= Left->AddressScale == Right->AddressScale;
      Same &= Left->IsLiteralCoordinate == Right->IsLiteralCoordinate;
      Same &= Left->LiteralCoordinate == Right->LiteralCoordinate;
      Same &= Left->AllowZeroExtension == Right->AllowZeroExtension;
      Same &= Left->AllowSignExtension == Right->AllowSignExtension;
      Same &= Left->HasBaseSelect == Right->HasBaseSelect;
      Same &= Left->HasBaseMaskBlend == Right->HasBaseMaskBlend;
      Same &= Left->TrueBase == Right->TrueBase;
      Same &= Left->FalseBase == Right->FalseBase;
      Same &= Left->PositiveBlendInputSide == Right->PositiveBlendInputSide;
      Same &= Left->PositiveBaseInputSide == Right->PositiveBaseInputSide;
      Same &= Left->NegativeBaseInputSide == Right->NegativeBaseInputSide;
    }
    const std::optional<bool> SameLoad = SameOccurrenceNoShortCircuit(
        Left ? &Left->Load : nullptr, Right ? &Right->Load : nullptr);
    if (!SameLoad)
      return std::nullopt;
    Same &= *SameLoad;
    static const std::vector<va_t> EmptyBases;
    const std::optional<bool> SameBases = SameScalarVectorNoShortCircuit(
        Left ? Left->AllowedBases : EmptyBases,
        Right ? Right->AllowedBases : EmptyBases);
    if (!SameBases)
      return std::nullopt;
    Same &= *SameBases;
    const std::optional<bool> SameFrame =
        SameFrameStorageNoShortCircuit(Left ? &Left->FrameStorage : nullptr,
                                       Right ? &Right->FrameStorage : nullptr);
    if (!SameFrame)
      return std::nullopt;
    Same &= *SameFrame;
    const std::optional<bool> SameDisplaced =
        SameDisplacedAddressNoShortCircuit(
            Left ? &Left->DisplacedAddress : nullptr,
            Right ? &Right->DisplacedAddress : nullptr);
    if (!SameDisplaced)
      return std::nullopt;
    Same &= *SameDisplaced;
    static const std::vector<JumpTableValueOccurrence> EmptyOccurrences;
    const std::optional<bool> SameIndices = SameOccurrenceVectorNoShortCircuit(
        Left ? Left->Indices : EmptyOccurrences,
        Right ? Right->Indices : EmptyOccurrences);
    if (!SameIndices)
      return std::nullopt;
    Same &= *SameIndices;
    const std::optional<bool> SameAddressIndex =
        SameOccurrenceNoShortCircuit(Left ? &Left->AddressIndex : nullptr,
                                     Right ? &Right->AddressIndex : nullptr);
    if (!SameAddressIndex)
      return std::nullopt;
    Same &= *SameAddressIndex;
    const JumpTableValueOccurrence *LeftOccurrences[] = {
        Left ? &Left->SelectedBase : nullptr,
        Left ? &Left->SelectCondition : nullptr,
        Left ? &Left->PositiveBlendArm : nullptr,
        Left ? &Left->NegativeBlendArm : nullptr,
        Left ? &Left->PositiveMask : nullptr,
        Left ? &Left->NegativeMask : nullptr};
    const JumpTableValueOccurrence *RightOccurrences[] = {
        Right ? &Right->SelectedBase : nullptr,
        Right ? &Right->SelectCondition : nullptr,
        Right ? &Right->PositiveBlendArm : nullptr,
        Right ? &Right->NegativeBlendArm : nullptr,
        Right ? &Right->PositiveMask : nullptr,
        Right ? &Right->NegativeMask : nullptr};
    for (size_t I = 0; I < std::size(LeftOccurrences); ++I) {
      const std::optional<bool> SameOccurrence =
          SameOccurrenceNoShortCircuit(LeftOccurrences[I], RightOccurrences[I]);
      if (!SameOccurrence)
        return std::nullopt;
      Same &= *SameOccurrence;
    }
    return Same;
  };
  auto SameLoadRoleVectorNoShortCircuit =
      [&](const auto &Left, const auto &Right) -> std::optional<bool> {
    if (!ConsumeProposalStageEvidence(1))
      return std::nullopt;
    bool Same = Left.size() == Right.size();
    for (size_t I = 0; I < std::max(Left.size(), Right.size()); ++I) {
      const auto *LeftRole = I < Left.size() ? &Left[I] : nullptr;
      const auto *RightRole = I < Right.size() ? &Right[I] : nullptr;
      const std::optional<bool> SameRole =
          SameLoadRoleNoShortCircuit(LeftRole, RightRole);
      if (!SameRole)
        return std::nullopt;
      Same &= *SameRole;
    }
    return Same;
  };
  auto SameJumpTableInfoNoShortCircuit =
      [&](const JumpTableInfo &Left,
          const JumpTableInfo &Right) -> std::optional<bool> {
    // Forty direct scalar fields plus optional-storage presence.  Each
    // dynamic container pays its size and maximum element traversal below.
    if (!ConsumeProposalStageEvidence(41))
      return std::nullopt;
    bool Same = true;
    Same &= Left.BaseAddr == Right.BaseAddr;
    Same &= Left.HasBaseAddr == Right.HasBaseAddr;
    Same &= Left.EntrySize == Right.EntrySize;
    Same &= Left.EntryStride == Right.EntryStride;
    Same &= Left.MaxEntries == Right.MaxEntries;
    Same &= Left.PhysicalCapacity == Right.PhysicalCapacity;
    Same &= Left.ExactBoundedRelativeRelocationSlots ==
            Right.ExactBoundedRelativeRelocationSlots;
    Same &= Left.IndexDomainAuthenticated == Right.IndexDomainAuthenticated;
    Same &= Left.AuthenticatedGuardBound == Right.AuthenticatedGuardBound;
    Same &= Left.AuthenticatedModuloBound == Right.AuthenticatedModuloBound;
    Same &=
        Left.AuthenticatedDenseMaskBound == Right.AuthenticatedDenseMaskBound;
    Same &= Left.ExactPhysicalStorageRange.has_value() ==
            Right.ExactPhysicalStorageRange.has_value();
    Same &= Left.IsRelative == Right.IsRelative;
    Same &= Left.IsSigned == Right.IsSigned;
    Same &= Left.RelocAbsolute == Right.RelocAbsolute;
    Same &= Left.RelocBounded == Right.RelocBounded;
    Same &= Left.TargetBase == Right.TargetBase;
    Same &= Left.HasTargetBase == Right.HasTargetBase;
    Same &= Left.IsPEImageRelativeRVA == Right.IsPEImageRelativeRVA;
    Same &= Left.EntryScale == Right.EntryScale;
    Same &= Left.NormBase == Right.NormBase;
    Same &= Left.NormShift == Right.NormShift;
    Same &= Left.Stride == Right.Stride;
    Same &= Left.PreScaledIndex == Right.PreScaledIndex;
    Same &= Left.UseSharedDispatchSelector == Right.UseSharedDispatchSelector;
    Same &= Left.TwoTableSelect == Right.TwoTableSelect;
    Same &= Left.CompositeShapeClaimed == Right.CompositeShapeClaimed;
    Same &= Left.TwoLevelIndex == Right.TwoLevelIndex;
    Same &= Left.TwoTableOffset == Right.TwoTableOffset;
    Same &= Left.TwoTableHiPositive == Right.TwoTableHiPositive;
    Same &= Left.MutatedUnsafe == Right.MutatedUnsafe;
    Same &= Left.IndexReg == Right.IndexReg;
    Same &= Left.IndexUseAddr == Right.IndexUseAddr;
    Same &= Left.IndexUseSeq == Right.IndexUseSeq;
    Same &= Left.IndexValueDefinedAtUse == Right.IndexValueDefinedAtUse;
    Same &= Left.TableLoadAddr == Right.TableLoadAddr;
    Same &= Left.TableLoadSeq == Right.TableLoadSeq;
    Same &= Left.RequiresCompleteCFGProof == Right.RequiresCompleteCFGProof;
    Same &= Left.HasControllingGuard == Right.HasControllingGuard;
    Same &= Left.IncompleteGuardDomain == Right.IncompleteGuardDomain;
    Same &=
        Left.SemanticGuardDomainAmbiguous == Right.SemanticGuardDomainAmbiguous;

    const std::optional<bool> SameIndexValue =
        SameNdVarNoShortCircuit(&Left.IndexValueAtUse, &Right.IndexValueAtUse);
    if (!SameIndexValue)
      return std::nullopt;
    Same &= *SameIndexValue;
    const std::optional<bool> SameCoordinates = SameScalarVectorNoShortCircuit(
        Left.AuthenticatedMaskCoordinates, Right.AuthenticatedMaskCoordinates);
    if (!SameCoordinates)
      return std::nullopt;
    Same &= *SameCoordinates;
    const std::optional<bool> SameWitnesses = SameWitnessVectorNoShortCircuit(
        Left.AuthenticatedMaskKnownOneWitnesses,
        Right.AuthenticatedMaskKnownOneWitnesses);
    if (!SameWitnesses)
      return std::nullopt;
    Same &= *SameWitnesses;
    const std::optional<bool> SameStorageRanges =
        SameStorageRangeVectorNoShortCircuit(Left.StorageRanges,
                                             Right.StorageRanges);
    if (!SameStorageRanges)
      return std::nullopt;
    Same &= *SameStorageRanges;
    if (Left.ExactPhysicalStorageRange || Right.ExactPhysicalStorageRange) {
      const std::optional<bool> SameExactStorage =
          SameStorageRangeNoShortCircuit(Left.ExactPhysicalStorageRange
                                             ? &*Left.ExactPhysicalStorageRange
                                             : nullptr,
                                         Right.ExactPhysicalStorageRange
                                             ? &*Right.ExactPhysicalStorageRange
                                             : nullptr);
      if (!SameExactStorage)
        return std::nullopt;
      Same &= *SameExactStorage;
    }
    const std::optional<bool> SameSuppression = SameScalarVectorNoShortCircuit(
        Left.SuppressibleRelocationSlots, Right.SuppressibleRelocationSlots);
    if (!SameSuppression)
      return std::nullopt;
    Same &= *SameSuppression;
    const std::optional<bool> SameAlternatives =
        SameOccurrenceVectorNoShortCircuit(Left.IndexValueAlternatives,
                                           Right.IndexValueAlternatives);
    if (!SameAlternatives)
      return std::nullopt;
    Same &= *SameAlternatives;
    const std::optional<bool> SameTargetLoads =
        SameOccurrenceVectorNoShortCircuit(Left.TargetLoads, Right.TargetLoads);
    if (!SameTargetLoads)
      return std::nullopt;
    Same &= *SameTargetLoads;
    const std::optional<bool> SameFrameStorage = SameFrameStorageNoShortCircuit(
        &Left.AuthenticatedFrameStorage, &Right.AuthenticatedFrameStorage);
    if (!SameFrameStorage)
      return std::nullopt;
    Same &= *SameFrameStorage;
    const std::optional<bool> SameDisplacedAddress =
        SameDisplacedAddressNoShortCircuit(
            &Left.AuthenticatedDisplacedAddress,
            &Right.AuthenticatedDisplacedAddress);
    if (!SameDisplacedAddress)
      return std::nullopt;
    Same &= *SameDisplacedAddress;
    const std::optional<bool> SameConsumers =
        SameOccurrenceVectorNoShortCircuit(Left.AuthenticatedStorageConsumers,
                                           Right.AuthenticatedStorageConsumers);
    if (!SameConsumers)
      return std::nullopt;
    Same &= *SameConsumers;
    const std::optional<bool> SameLoadRoles =
        SameLoadRoleVectorNoShortCircuit(Left.LoadRoles, Right.LoadRoles);
    if (!SameLoadRoles)
      return std::nullopt;
    Same &= *SameLoadRoles;
    const std::optional<bool> SameEntryIndices =
        SameScalarVectorNoShortCircuit(Left.EntryIndices, Right.EntryIndices);
    if (!SameEntryIndices)
      return std::nullopt;
    Same &= *SameEntryIndices;
    const std::optional<bool> SameRuntimeLabels =
        SameScalarVectorNoShortCircuit(Left.RuntimeCaseLabels,
                                       Right.RuntimeCaseLabels);
    if (!SameRuntimeLabels)
      return std::nullopt;
    Same &= *SameRuntimeLabels;
    const std::optional<bool> SameRuntimeSlots = SameScalarVectorNoShortCircuit(
        Left.RuntimeSlotIndices, Right.RuntimeSlotIndices);
    if (!SameRuntimeSlots)
      return std::nullopt;
    Same &= *SameRuntimeSlots;
    const std::optional<bool> SameExplicitTargets =
        SameScalarVectorNoShortCircuit(Left.ExplicitTargets,
                                       Right.ExplicitTargets);
    if (!SameExplicitTargets)
      return std::nullopt;
    Same &= *SameExplicitTargets;

    // CircleRange equality treats all empty ranges as equal, independent of
    // their inactive payload.  Evaluate every public field after reserving
    // the full five-comparison upper bound, then preserve that semantics.
    if (!ConsumeProposalStageEvidence(5))
      return std::nullopt;
    const bool LeftEmpty = Left.GuardRange.isEmpty();
    const bool RightEmpty = Right.GuardRange.isEmpty();
    bool SameActiveRange = true;
    SameActiveRange &= Left.GuardRange.getMin() == Right.GuardRange.getMin();
    SameActiveRange &= Left.GuardRange.getEnd() == Right.GuardRange.getEnd();
    SameActiveRange &= Left.GuardRange.getMask() == Right.GuardRange.getMask();
    SameActiveRange &= Left.GuardRange.getStep() == Right.GuardRange.getStep();
    Same &= LeftEmpty == RightEmpty;
    Same &= (LeftEmpty && RightEmpty) ||
            (!LeftEmpty && !RightEmpty && SameActiveRange);
    return Same;
  };
  return SameJumpTableInfoNoShortCircuit(Left, Right);
}

} // namespace neverd
