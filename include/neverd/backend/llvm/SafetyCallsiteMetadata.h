//===- SafetyCallsiteMetadata.h - Counted-write call identity ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the persistent, format-neutral metadata that joins one exact MedIR
/// counted-write occurrence to its emitted LLVM CallBase.  This header lives
/// below the safety analysis layer deliberately: the LLVM backend can produce
/// the contract without depending on a safety planner, while later consumers
/// can parse the same closed typed value.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_SAFETYCALLSITEMETADATA_H
#define NEVERD_BACKEND_LLVM_SAFETYCALLSITEMETADATA_H

#include "neverd/safety/CountedWriteSemantics.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <system_error>

namespace neverd::safety_callsite_md {

inline constexpr llvm::StringLiteral Attachment("neverd.safety.callsite.v1");
inline constexpr uint32_t SchemaVersion = 1;

using SemanticKind = counted_write::SemanticKind;

struct SafetyCallsiteOccurrence {
  uint64_t FuncEntry = 0;
  uint64_t CallVA = 0;
  uint32_t BlockId = 0;
  uint32_t OpIdx = 0;
  uint32_t OriginSeq = 0;
  uint32_t CallSiteId = 0;

  friend bool operator==(const SafetyCallsiteOccurrence &Left,
                         const SafetyCallsiteOccurrence &Right) {
    return Left.FuncEntry == Right.FuncEntry && Left.CallVA == Right.CallVA &&
           Left.BlockId == Right.BlockId && Left.OpIdx == Right.OpIdx &&
           Left.OriginSeq == Right.OriginSeq &&
           Left.CallSiteId == Right.CallSiteId;
  }
  friend bool operator!=(const SafetyCallsiteOccurrence &Left,
                         const SafetyCallsiteOccurrence &Right) {
    return !(Left == Right);
  }
};

struct SafetyCallsiteRecord {
  SafetyCallsiteOccurrence Occurrence;
  SemanticKind Kind = SemanticKind::Memcpy;
  uint32_t DestinationOperandIndex = 0;
  uint32_t LengthOperandIndex = 0;
  uint32_t ElementBytes = 1;

  friend bool operator==(const SafetyCallsiteRecord &Left,
                         const SafetyCallsiteRecord &Right) {
    return Left.Occurrence == Right.Occurrence && Left.Kind == Right.Kind &&
           Left.DestinationOperandIndex == Right.DestinationOperandIndex &&
           Left.LengthOperandIndex == Right.LengthOperandIndex &&
           Left.ElementBytes == Right.ElementBytes;
  }
  friend bool operator!=(const SafetyCallsiteRecord &Left,
                         const SafetyCallsiteRecord &Right) {
    return !(Left == Right);
  }
};

enum Operand : unsigned {
  Version = 0,                 // i32
  FuncEntry = 1,               // i64
  CallVA = 2,                  // i64
  BlockId = 3,                 // i32
  OpIdx = 4,                   // i32
  OriginSeq = 5,               // i32
  CallSiteId = 6,              // i32
  Kind = 7,                    // i32
  DestinationOperandIndex = 8, // i32
  LengthOperandIndex = 9,      // i32
  ElementBytes = 10,           // i32
  OperandCount = 11,
};
static_assert(OperandCount == 11);

namespace detail {

inline llvm::Error invalidMetadata(llvm::StringRef Reason) {
  return llvm::createStringError(
      std::make_error_code(std::errc::invalid_argument),
      (llvm::Twine("safety callsite metadata ") + Reason).str());
}

inline llvm::Error validate(const llvm::CallBase &Call,
                            const SafetyCallsiteRecord &Record) {
  const SafetyCallsiteOccurrence &Occurrence = Record.Occurrence;
  constexpr uint32_t MaxSignedIndex =
      static_cast<uint32_t>(std::numeric_limits<int32_t>::max());
  if (Occurrence.FuncEntry == std::numeric_limits<uint64_t>::max() ||
      Occurrence.CallVA == std::numeric_limits<uint64_t>::max() ||
      Occurrence.BlockId > MaxSignedIndex ||
      Occurrence.OpIdx > MaxSignedIndex ||
      Occurrence.OriginSeq > MaxSignedIndex || Occurrence.CallSiteId == 0)
    return invalidMetadata("has an incomplete occurrence identity");
  if (Record.DestinationOperandIndex == Record.LengthOperandIndex ||
      Record.DestinationOperandIndex >= Call.arg_size() ||
      Record.LengthOperandIndex >= Call.arg_size())
    return invalidMetadata("has invalid emitted operand indexes");
  if (!counted_write::isValid({Record.Kind, Record.DestinationOperandIndex,
                               Record.LengthOperandIndex, Record.ElementBytes}))
    return invalidMetadata("has invalid counted-write semantics");
  return llvm::Error::success();
}

} // namespace detail

/// Attach one v1 tuple.  Validation is transactional: an invalid record or an
/// existing attachment returns an error without changing the instruction.
inline llvm::Error attach(llvm::CallBase &Call,
                          const SafetyCallsiteRecord &Record) {
  if (llvm::Error Error = detail::validate(Call, Record))
    return Error;
  if (Call.getMetadata(Attachment))
    return detail::invalidMetadata("is already attached");

  llvm::LLVMContext &Context = Call.getContext();
  auto UInt = [&](uint64_t Value, unsigned Width) -> llvm::Metadata * {
    return llvm::ConstantAsMetadata::get(
        llvm::ConstantInt::get(llvm::IntegerType::get(Context, Width), Value));
  };
  Call.setMetadata(
      Attachment,
      llvm::MDNode::get(Context, {UInt(SchemaVersion, 32),
                                  UInt(Record.Occurrence.FuncEntry, 64),
                                  UInt(Record.Occurrence.CallVA, 64),
                                  UInt(Record.Occurrence.BlockId, 32),
                                  UInt(Record.Occurrence.OpIdx, 32),
                                  UInt(Record.Occurrence.OriginSeq, 32),
                                  UInt(Record.Occurrence.CallSiteId, 32),
                                  UInt(static_cast<uint32_t>(Record.Kind), 32),
                                  UInt(Record.DestinationOperandIndex, 32),
                                  UInt(Record.LengthOperandIndex, 32),
                                  UInt(Record.ElementBytes, 32)}));
  return llvm::Error::success();
}

/// Parse one v1 tuple.  A missing attachment is a successful nullopt; a
/// present malformed tuple is an Error and is never partially recovered.
inline llvm::Expected<std::optional<SafetyCallsiteRecord>>
parse(const llvm::CallBase &Call) {
  const llvm::MDNode *Node = Call.getMetadata(Attachment);
  if (!Node)
    return std::optional<SafetyCallsiteRecord>();
  if (Node->getNumOperands() != OperandCount)
    return detail::invalidMetadata("has an invalid operand count");

  auto ReadUInt = [&](unsigned Index,
                      unsigned Width) -> std::optional<uint64_t> {
    const auto *Metadata = llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(
        Node->getOperand(Index).get());
    const auto *Integer =
        Metadata ? llvm::dyn_cast<llvm::ConstantInt>(Metadata->getValue())
                 : nullptr;
    if (!Integer || Integer->getBitWidth() != Width)
      return std::nullopt;
    return Integer->getZExtValue();
  };

  const std::optional<uint64_t> VersionValue = ReadUInt(Version, 32);
  const std::optional<uint64_t> FuncEntryValue = ReadUInt(FuncEntry, 64);
  const std::optional<uint64_t> CallVAValue = ReadUInt(CallVA, 64);
  const std::optional<uint64_t> BlockIdValue = ReadUInt(BlockId, 32);
  const std::optional<uint64_t> OpIdxValue = ReadUInt(OpIdx, 32);
  const std::optional<uint64_t> OriginSeqValue = ReadUInt(OriginSeq, 32);
  const std::optional<uint64_t> CallSiteIdValue = ReadUInt(CallSiteId, 32);
  const std::optional<uint64_t> KindValue = ReadUInt(Kind, 32);
  const std::optional<uint64_t> DestinationValue =
      ReadUInt(DestinationOperandIndex, 32);
  const std::optional<uint64_t> LengthValue = ReadUInt(LengthOperandIndex, 32);
  const std::optional<uint64_t> ElementBytesValue = ReadUInt(ElementBytes, 32);
  if (!VersionValue || *VersionValue != SchemaVersion || !FuncEntryValue ||
      !CallVAValue || !BlockIdValue || !OpIdxValue || !OriginSeqValue ||
      !CallSiteIdValue || !KindValue || !DestinationValue || !LengthValue ||
      !ElementBytesValue)
    return detail::invalidMetadata("has an invalid schema or integer width");

  SafetyCallsiteRecord Record;
  Record.Occurrence.FuncEntry = *FuncEntryValue;
  Record.Occurrence.CallVA = *CallVAValue;
  Record.Occurrence.BlockId = static_cast<uint32_t>(*BlockIdValue);
  Record.Occurrence.OpIdx = static_cast<uint32_t>(*OpIdxValue);
  Record.Occurrence.OriginSeq = static_cast<uint32_t>(*OriginSeqValue);
  Record.Occurrence.CallSiteId = static_cast<uint32_t>(*CallSiteIdValue);
  Record.Kind = static_cast<SemanticKind>(*KindValue);
  Record.DestinationOperandIndex = static_cast<uint32_t>(*DestinationValue);
  Record.LengthOperandIndex = static_cast<uint32_t>(*LengthValue);
  Record.ElementBytes = static_cast<uint32_t>(*ElementBytesValue);
  if (llvm::Error Error = detail::validate(Call, Record))
    return std::move(Error);
  return std::optional<SafetyCallsiteRecord>(Record);
}

} // namespace neverd::safety_callsite_md

#endif // NEVERD_BACKEND_LLVM_SAFETYCALLSITEMETADATA_H
