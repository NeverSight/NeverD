//===- COFFExceptionPatch.cpp - Safe PE exception-table rewrite ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/COFF/COFFExceptionPatch.h"

#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/COFF/COFFFH4Encoding.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/WindowsEHMetadataEncoder.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFException.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/object/PELayout.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/ARMWinEH.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/Win64EH.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {
namespace {

constexpr uint64_t X64RuntimeFunctionSize = 3 * sizeof(uint32_t);
constexpr uint64_t ARMRuntimeFunctionSize = 2 * sizeof(uint32_t);
constexpr uint32_t MaxGeneratedLanguageRecords = 1u << 16;
constexpr uint32_t FH3MagicNumber3 = 0x19930522u;
constexpr size_t FH3FuncInfoSize = 10 * sizeof(uint32_t);
constexpr size_t FH3TryBlockSize = 5 * sizeof(uint32_t);
constexpr size_t FH3HandlerTypeSize = 5 * sizeof(uint32_t);
constexpr size_t FH3StateMapEntrySize = 2 * sizeof(uint32_t);
constexpr uint32_t ReturnFlowGuardMask = 0x000e0000u;
constexpr uint32_t RetpolinePresent = 0x00100000u;
constexpr uint32_t XFGEnabled = 0x00800000u;

bool isFH3OutputArchitecture(Arch TargetArch) {
  return TargetArch == Arch::X64 || TargetArch == Arch::AArch64;
}

llvm::Error patchError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "coff exception patch: " + Message);
}

llvm::Expected<coff_fh4::FuncInfoLayout> parseGeneratedFH4Layout(
    uint32_t FuncInfoRVA,
    llvm::function_ref<const uint8_t *(uint32_t, size_t)> ReadAtRVA) {
  return coff_fh4::parseFuncInfoLayout(
      FuncInfoRVA,
      [&](uint32_t RVA,
          uint32_t Size) -> llvm::Expected<llvm::ArrayRef<uint8_t>> {
        const uint8_t *Bytes = ReadAtRVA(RVA, Size);
        if (!Bytes)
          return patchError("generated C++ EH4 bytes are not mapped");
        return llvm::ArrayRef<uint8_t>(Bytes, Size);
      },
      {/*MaxRecords=*/MaxGeneratedLanguageRecords,
       /*MaxBytes=*/1u << 20});
}

struct BoundedFH4HandlerRow {
  uint8_t Header = 0;
  uint32_t Adjectives = 0;
  uint32_t TypeDescriptorRVA = 0;
  uint32_t HandlerRVA = 0;
};

llvm::Expected<BoundedFH4HandlerRow>
parseBoundedFH4HandlerRow(llvm::ArrayRef<uint8_t> Bytes) {
  if (Bytes.empty())
    return patchError("generated C++ EH4 handler row is empty");

  BoundedFH4HandlerRow Result;
  Result.Header = Bytes.front();
  if ((Result.Header & ~uint8_t(0x03)) != 0)
    return patchError(
        "generated C++ EH4 handler row exceeds the bounded model");

  size_t Offset = 1;
  if ((Result.Header & 0x01) != 0) {
    auto Adjectives = coff_fh4::decodeCompressedUInt(Bytes.drop_front(Offset));
    if (!Adjectives)
      return Adjectives.takeError();
    if (Adjectives->Value == 0)
      return patchError(
          "generated C++ EH4 handler row has a redundant adjective field");
    Result.Adjectives = Adjectives->Value;
    Offset += Adjectives->Size;
  }
  if ((Result.Header & 0x02) != 0) {
    if (Offset > Bytes.size() || Bytes.size() - Offset < sizeof(uint32_t))
      return patchError("generated C++ EH4 typed handler row is truncated");
    Result.TypeDescriptorRVA = readLE<uint32_t>(Bytes.data() + Offset);
    if (Result.TypeDescriptorRVA == 0)
      return patchError(
          "generated C++ EH4 typed handler has a zero type descriptor");
    Offset += sizeof(uint32_t);
  } else if (Result.Header != 0x01 || Result.Adjectives != 0x40) {
    return patchError(
        "generated C++ EH4 untyped handler is not a canonical catch-all");
  }

  if (Offset > Bytes.size() || Bytes.size() - Offset != sizeof(uint32_t))
    return patchError("generated C++ EH4 handler row has an invalid extent");
  Result.HandlerRVA = readLE<uint32_t>(Bytes.data() + Offset);
  if (Result.HandlerRVA == 0)
    return patchError("generated C++ EH4 handler row has a zero handler");
  return Result;
}

llvm::Error
validateBoundedGeneratedFH4Layout(const coff_fh4::FuncInfoLayout &Layout) {
  if (Layout.Header != 0x38 || Layout.BBTFlags != 0 ||
      Layout.FrameOffset != 0 || Layout.SeparatedStates || !Layout.Unwind ||
      !Layout.Try || !Layout.States || Layout.Unwind->Entries.size() != 2 ||
      Layout.Try->Entries.size() != 1 || Layout.States->Entries.size() != 3)
    return patchError("generated C++ EH4 layout exceeds the bounded model");

  for (const coff_fh4::UnwindEntry &Entry : Layout.Unwind->Entries)
    if (Entry.ToState != -1 || Entry.Kind != coff_fh4::UnwindActionKind::None ||
        Entry.ActionRVA != 0 || Entry.ObjectOffset != 0)
      return patchError("generated C++ EH4 unwind map changed semantics");

  const coff_fh4::TryEntry &Try = Layout.Try->Entries.front();
  if (Try.TryLow != 0 || Try.TryHigh != 0 || Try.CatchHigh != 1 ||
      Try.Handlers.Entries.size() != 1)
    return patchError("generated C++ EH4 try map changed semantics");
  const coff_fh4::HandlerEntry &Handler = Try.Handlers.Entries.front();
  const bool IsTyped = Handler.TypeDescriptorRVA != 0;
  const uint8_t ExpectedHeader = uint8_t(IsTyped ? 0x02 : 0x01) |
                                 uint8_t(Handler.Adjectives != 0 ? 0x01 : 0);
  const size_t ExpectedSize =
      1 +
      (Handler.Adjectives == 0
           ? 0
           : coff_fh4::encodeCompressedUInt(Handler.Adjectives).size()) +
      (IsTyped ? sizeof(uint32_t) : 0) + sizeof(uint32_t);
  if (Handler.Header != ExpectedHeader ||
      (!IsTyped && Handler.Adjectives != 0x40) ||
      Handler.CatchObjectOffset != 0 || Handler.HandlerRVA == 0 ||
      !Handler.Continuations.empty() || Handler.Range.size() != ExpectedSize)
    return patchError("generated C++ EH4 handler map changed semantics");

  const auto &States = Layout.States->Entries;
  if (States[0].State != -1 || States[0].FunctionOffset != 0 ||
      States[1].State != 0 || States[2].State != -1 ||
      States[1].FunctionOffset == 0 ||
      States[1].FunctionOffset >= States[2].FunctionOffset)
    return patchError("generated C++ EH4 IP map changed semantics");
  return llvm::Error::success();
}

bool hasUnsupportedGeneratedGuardMode(uint32_t GuardFlags) {
  const uint32_t Unsupported =
      uint32_t(llvm::COFF::GuardFlags::CFW_INSTRUMENTED) | ReturnFlowGuardMask |
      RetpolinePresent | XFGEnabled;
  return (GuardFlags & Unsupported) != 0;
}

std::optional<uint64_t> metadataUInt(const llvm::MDNode &Node, unsigned Index) {
  if (Index >= Node.getNumOperands())
    return std::nullopt;
  auto *CAM = llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(
      Node.getOperand(Index).get());
  auto *CI = CAM ? llvm::dyn_cast<llvm::ConstantInt>(CAM->getValue()) : nullptr;
  if (!CI || CI->getBitWidth() > 64)
    return std::nullopt;
  return CI->getZExtValue();
}

std::optional<llvm::StringRef> metadataString(const llvm::MDNode &Node,
                                              unsigned Index) {
  if (Index >= Node.getNumOperands())
    return std::nullopt;
  auto *S =
      llvm::dyn_cast_or_null<llvm::MDString>(Node.getOperand(Index).get());
  if (!S)
    return std::nullopt;
  return S->getString();
}

std::optional<llvm::mc_rewrite::RewriteWinEHSemanticToken>
parseRewriteWinEHSemanticToken(
    const llvm::Instruction &Pad,
    llvm::mc_rewrite::RewriteWinEHSemanticKind ExpectedKind) {
  const llvm::MDNode *Metadata =
      Pad.getMetadata(llvm::mc_rewrite::RewriteWinEHSemanticAttachment);
  if (!Metadata || Metadata->getNumOperands() !=
                       llvm::mc_rewrite::RewriteWinEHSemanticOperandCount)
    return std::nullopt;

  auto GetUInt = [&](unsigned Index,
                     unsigned Width) -> std::optional<uint64_t> {
    const auto *CAM = llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(
        Metadata->getOperand(Index).get());
    const auto *CI =
        CAM ? llvm::dyn_cast<llvm::ConstantInt>(CAM->getValue()) : nullptr;
    if (!CI || CI->getBitWidth() != Width)
      return std::nullopt;
    return CI->getZExtValue();
  };

  const std::optional<uint64_t> Version = GetUInt(0, 32);
  const std::optional<uint64_t> Kind = GetUInt(1, 8);
  const std::optional<uint64_t> Region = GetUInt(2, 32);
  const std::optional<uint64_t> Clause = GetUInt(3, 32);
  if (!Version ||
      *Version != llvm::mc_rewrite::RewriteWinEHSemanticSchemaVersion ||
      !Kind || *Kind != static_cast<uint8_t>(ExpectedKind) || !Region ||
      !Clause)
    return std::nullopt;

  llvm::mc_rewrite::RewriteWinEHSemanticToken Token;
  Token.Kind = ExpectedKind;
  Token.Region = static_cast<uint32_t>(*Region);
  Token.Clause = static_cast<uint32_t>(*Clause);
  for (unsigned I = 0; I != Token.Digest.size(); ++I) {
    const std::optional<uint64_t> Digest = GetUInt(4 + I, 64);
    if (!Digest)
      return std::nullopt;
    Token.Digest[I] = *Digest;
  }
  return Token;
}

std::optional<va_t> autoFunctionAddress(llvm::StringRef Name) {
  if (!Name.consume_front(kAutoFuncPrefix) || Name.empty())
    return std::nullopt;
  va_t Address = 0;
  if (Name.getAsInteger(16, Address))
    return std::nullopt;
  return Address;
}

struct CanonicalWindowsEHFunction {
  const llvm::Function *Function = nullptr;
  const ExceptionFunction *Source = nullptr;
};

/// Authenticate the three representations of every lifted Windows exception
/// function before any patch planning is allowed to use one of them:
///
///   * the function attachment,
///   * its exact row in the module-level function table, and
///   * a canonical current-schema re-encoding of the primary source-image
///     record.
///
/// RewriteSourceIdentity is the only join key.  Function names and selected
/// scalar attachment operands are deliberately insufficient: either would let
/// a structurally changed nested record retain the same apparent entry.
llvm::Expected<std::vector<CanonicalWindowsEHFunction>>
validateCanonicalWindowsEHIdentity(const llvm::Module &Mod,
                                   const BinaryImage &Image) {
  std::vector<CanonicalWindowsEHFunction> Functions;
  std::map<const llvm::Function *, const llvm::MDNode *> Attachments;
  std::vector<const llvm::Function *> AttachedDeclarations;
  std::set<va_t> ClaimedPrimaryEntries;

  auto FindPrimary = [&](va_t Entry, llvm::StringRef FunctionName)
      -> llvm::Expected<const ExceptionFunction *> {
    const ExceptionFunction *Primary = nullptr;
    for (const ExceptionFunction &EH : Image.ExceptionMetadata.Functions) {
      if (EH.Kind != RuntimeFunctionKind::Primary ||
          EH.CodeRange.Begin != Entry)
        continue;
      if (Primary)
        return patchError(
            "rewrite source identity names more than one primary Windows EH "
            "record for function " +
            FunctionName);
      Primary = &EH;
    }
    if (!Primary)
      return patchError(
          "rewrite source identity does not name a primary Windows EH record "
          "for function " +
          FunctionName);
    return Primary;
  };

  for (const llvm::Function &F : Mod) {
    const llvm::MDNode *Attachment =
        F.getMetadata(windows_eh_md::FunctionAttachment);
    auto SourceIdentity = rewrite_source::getOriginalVA(F);
    if (!SourceIdentity)
      return patchError("invalid rewrite source identity on function " +
                        F.getName() + ": " +
                        llvm::toString(SourceIdentity.takeError()));

    if (!Attachment) {
      // A defined lifted source which maps exactly to a primary runtime record
      // may not silently omit its exception contract.  Retain the historical
      // auto-name check for manually supplied modules without source identity.
      if (!F.isDeclaration() && *SourceIdentity) {
        const size_t PrimaryMatches = static_cast<size_t>(std::count_if(
            Image.ExceptionMetadata.Functions.begin(),
            Image.ExceptionMetadata.Functions.end(),
            [&](const ExceptionFunction &EH) {
              return EH.Kind == RuntimeFunctionKind::Primary &&
                     EH.CodeRange.Begin == **SourceIdentity;
            }));
        if (PrimaryMatches != 0)
          return patchError("function " + F.getName() +
                            " omits its Windows EH metadata");
      } else if (!F.isDeclaration()) {
        if (auto Address = autoFunctionAddress(F.getName()))
          if (const ExceptionFunction *EH =
                  Image.ExceptionMetadata.findFunction(*Address)) {
            (void)EH;
            return patchError("function " + F.getName() +
                              " omits its Windows EH metadata");
          }
      }
      continue;
    }

    Attachments.emplace(&F, Attachment);
    if (F.isDeclaration()) {
      AttachedDeclarations.push_back(&F);
      continue;
    }
    if (!*SourceIdentity || **SourceIdentity == InvalidVA)
      return patchError("Windows EH function lacks an exact rewrite source "
                        "identity: " +
                        F.getName());

    auto Primary = FindPrimary(**SourceIdentity, F.getName());
    if (!Primary)
      return Primary.takeError();
    llvm::MDNode *Canonical = windows_eh_md::getCanonicalFunctionMetadata(
        Mod.getContext(), **Primary, Image.Arch, BinaryFormat::COFF);
    if (Attachment != Canonical)
      return patchError("Windows EH metadata does not match the input image "
                        "for function " +
                        F.getName());
    if (!ClaimedPrimaryEntries.insert((*Primary)->CodeRange.Begin).second)
      return patchError("more than one IR function names Windows EH source "
                        "entry 0x" +
                        llvm::utohexstr((*Primary)->CodeRange.Begin));
    Functions.push_back({&F, *Primary});
  }

  const llvm::NamedMDNode *Table =
      Mod.getNamedMetadata(windows_eh_md::FunctionTable);
  if (!Attachments.empty() && !Table)
    return patchError("module omits the Windows EH function table");
  if (!Table)
    return Functions;
  if (!Attachments.empty() && Table->getNumOperands() == 0)
    return patchError("Windows EH function table is empty while function "
                      "attachments are present");

  std::map<const llvm::Function *, const llvm::MDNode *> Rows;
  for (const llvm::MDNode *Row : Table->operands()) {
    if (!Row || Row->getNumOperands() != 2)
      return patchError("malformed Windows EH function-table row");
    const auto *FunctionValue = llvm::dyn_cast_or_null<llvm::ValueAsMetadata>(
        Row->getOperand(0).get());
    const auto *Function =
        FunctionValue
            ? llvm::dyn_cast<llvm::Function>(FunctionValue->getValue())
            : nullptr;
    const auto *Payload =
        llvm::dyn_cast_or_null<llvm::MDNode>(Row->getOperand(1).get());
    if (!Function || !Payload)
      return patchError("malformed Windows EH function-table row");
    if (Function->getParent() != &Mod)
      return patchError("Windows EH function-table row references external "
                        "function " +
                        Function->getName());

    auto Attachment = Attachments.find(Function);
    if (Attachment == Attachments.end())
      return patchError("orphan Windows EH function-table row for function " +
                        Function->getName());
    if (Payload != Attachment->second)
      return patchError("Windows EH function-table payload does not match the "
                        "function attachment for function " +
                        Function->getName());
    if (Function->isDeclaration())
      return patchError("Windows EH function-table row references declaration " +
                        Function->getName());
    if (!Rows.emplace(Function, Payload).second)
      return patchError("duplicate Windows EH function-table row for function " +
                        Function->getName());
  }

  for (const CanonicalWindowsEHFunction &Entry : Functions)
    if (!Rows.count(Entry.Function))
      return patchError("Windows EH function table omits attached function " +
                        Entry.Function->getName());
  if (!AttachedDeclarations.empty())
    return patchError("Windows EH attachment is present on declaration " +
                      AttachedDeclarations.front()->getName());
  return Functions;
}

bool hasNativeEHMarker(const llvm::Function &F, llvm::StringRef Kind) {
  const llvm::MDNode *Marker = F.getMetadata(windows_eh_md::NativeAttachment);
  return Marker && Marker->getNumOperands() == 2 &&
         metadataUInt(*Marker, 0).value_or(0) == 1 &&
         metadataString(*Marker, 1).value_or("") == Kind;
}

bool hasPersonality(const llvm::Function &F, llvm::StringRef Name) {
  if (!F.hasPersonalityFn())
    return false;
  const llvm::Value *Personality = F.getPersonalityFn()->stripPointerCasts();
  const auto *PersonalityFunction = llvm::dyn_cast<llvm::Function>(Personality);
  return PersonalityFunction && PersonalityFunction->getName() == Name;
}

bool isExactExternalWinEHDeclaration(const llvm::Function *F,
                                     llvm::StringRef Name) {
  if (!F || F->getName() != Name || !F->isDeclaration() ||
      !F->hasExternalLinkage() ||
      F->getVisibility() != llvm::GlobalValue::DefaultVisibility ||
      F->getCallingConv() != llvm::CallingConv::C || F->getAddressSpace() != 0)
    return false;
  const llvm::FunctionType *Type = F->getFunctionType();
  return Type->getReturnType()->isIntegerTy(32) && Type->getNumParams() == 0 &&
         Type->isVarArg();
}

bool isCxxFH4SourcePersonality(ExceptionPersonality Personality) {
  return Personality == ExceptionPersonality::CxxFrameHandler4 ||
         Personality == ExceptionPersonality::GSHandlerCheckEH4;
}

bool isValidBoundedX64GSCookieHeader(uint32_t Header) {
  const uint32_t Offset = Header & ~uint32_t(7);
  return Offset != 0 && Offset <= std::numeric_limits<int32_t>::max() &&
         (Header & 7u) == 3u;
}

bool hasExactReconstructedGSCookie(const ExceptionFunction &EH,
                                   uint32_t Header) {
  if (!isValidBoundedX64GSCookieHeader(Header) || !EH.GSCookie)
    return false;
  const GSCookieInfo &Cookie = *EH.GSCookie;
  return Cookie.ParseStatus == ExceptionParseStatus::Complete &&
         Cookie.CookieOffset == static_cast<int32_t>(Header & ~uint32_t(7)) &&
         Cookie.HasExceptionHandler && Cookie.HasUnwindHandler &&
         !Cookie.HasAlignment && Cookie.AlignmentBaseOffset == 0 &&
         Cookie.Alignment == 0 && Cookie.Payload.size() == sizeof(uint32_t) &&
         readLE<uint32_t>(Cookie.Payload.data()) == Header;
}

const llvm::Instruction *
nextSemanticInstruction(const llvm::Instruction &Instruction) {
  const llvm::Instruction *Next = Instruction.getNextNode();
  while (Next && Next->isDebugOrPseudoInst())
    Next = Next->getNextNode();
  return Next;
}

const llvm::Instruction *
previousSemanticInstruction(const llvm::Instruction &Instruction) {
  const llvm::Instruction *Previous = Instruction.getPrevNode();
  while (Previous && Previous->isDebugOrPseudoInst())
    Previous = Previous->getPrevNode();
  return Previous;
}

std::optional<uint64_t> provenanceUInt(const llvm::OperandBundleUse &Bundle,
                                       unsigned Index, unsigned BitWidth) {
  if (Index >= Bundle.Inputs.size())
    return std::nullopt;
  const auto *Value =
      llvm::dyn_cast<llvm::ConstantInt>(Bundle.Inputs[Index].get());
  if (!Value || Value->getBitWidth() != BitWidth)
    return std::nullopt;
  return Value->getZExtValue();
}

struct NativeEHProvenance {
  windows_eh_md::NativeProvenanceModel Model =
      windows_eh_md::NativeProvenanceModel::SEH;
  windows_eh_md::NativeProvenanceRole Role =
      windows_eh_md::NativeProvenanceRole::ProtectedInvoke;
  va_t FunctionVA = 0;
  va_t SourceVA = 0;
  uint32_t Region = 0;
  uint32_t Clause = 0;
  va_t AuxVA = 0;
  uint32_t Flags = 0;
};

std::optional<NativeEHProvenance>
parseNativeEHProvenance(const llvm::CallInst &Anchor) {
  const llvm::Function *Callee = Anchor.getCalledFunction();
  if (!Callee || Callee->getIntrinsicID() != llvm::Intrinsic::sideeffect ||
      Anchor.countOperandBundlesOfType(windows_eh_md::ProvenanceBundle) != 1)
    return std::nullopt;
  auto Bundle = Anchor.getOperandBundle(windows_eh_md::ProvenanceBundle);
  if (!Bundle || Bundle->Inputs.size() != windows_eh_md::ProvenanceOperandCount)
    return std::nullopt;

  auto Version = provenanceUInt(*Bundle, windows_eh_md::ProvenanceVersion, 32);
  auto Model = provenanceUInt(*Bundle, windows_eh_md::ProvenanceModel, 8);
  auto Role = provenanceUInt(*Bundle, windows_eh_md::ProvenanceRole, 8);
  auto FunctionVA =
      provenanceUInt(*Bundle, windows_eh_md::ProvenanceFunctionVA, 64);
  auto SourceVA =
      provenanceUInt(*Bundle, windows_eh_md::ProvenanceSourceVA, 64);
  auto Region = provenanceUInt(*Bundle, windows_eh_md::ProvenanceRegion, 32);
  auto Clause = provenanceUInt(*Bundle, windows_eh_md::ProvenanceClause, 32);
  auto AuxVA = provenanceUInt(*Bundle, windows_eh_md::ProvenanceAuxVA, 64);
  auto Flags = provenanceUInt(*Bundle, windows_eh_md::ProvenanceFlags, 32);
  if (!Version || *Version != windows_eh_md::ProvenanceSchemaVersion ||
      !Model || !Role || !FunctionVA || !SourceVA || !Region || !Clause ||
      !AuxVA || !Flags)
    return std::nullopt;
  if (*Model !=
          static_cast<unsigned>(windows_eh_md::NativeProvenanceModel::SEH) &&
      *Model !=
          static_cast<unsigned>(windows_eh_md::NativeProvenanceModel::CxxFH3) &&
      *Model !=
          static_cast<unsigned>(windows_eh_md::NativeProvenanceModel::CxxFH4))
    return std::nullopt;
  if (*Role != static_cast<unsigned>(
                   windows_eh_md::NativeProvenanceRole::ProtectedInvoke) &&
      *Role != static_cast<unsigned>(
                   windows_eh_md::NativeProvenanceRole::RegionDispatch) &&
      *Role != static_cast<unsigned>(
                   windows_eh_md::NativeProvenanceRole::HandlerTarget) &&
      *Role != static_cast<unsigned>(
                   windows_eh_md::NativeProvenanceRole::RangeEnter) &&
      *Role != static_cast<unsigned>(
                   windows_eh_md::NativeProvenanceRole::RangeExit) &&
      *Role != static_cast<unsigned>(
                   windows_eh_md::NativeProvenanceRole::RangeEnterTarget) &&
      *Role != static_cast<unsigned>(
                   windows_eh_md::NativeProvenanceRole::RangeExitTarget))
    return std::nullopt;

  return NativeEHProvenance{
      static_cast<windows_eh_md::NativeProvenanceModel>(*Model),
      static_cast<windows_eh_md::NativeProvenanceRole>(*Role),
      *FunctionVA,
      *SourceVA,
      static_cast<uint32_t>(*Region),
      static_cast<uint32_t>(*Clause),
      *AuxVA,
      static_cast<uint32_t>(*Flags)};
}

struct NativeEHClause {
  const llvm::CatchPadInst *CatchPad = nullptr;
  const llvm::CleanupPadInst *CleanupPad = nullptr;
  va_t SourceVA = 0;
  va_t AuxVA = 0;
  uint32_t Flags = 0;
};

struct NativeEHDispatch {
  const llvm::BasicBlock *Destination = nullptr;
  bool IsCleanup = false;
  std::set<uint32_t> Clauses;
  std::map<uint32_t, const llvm::BasicBlock *> Continuations;
  std::map<uint32_t, NativeEHClause> ClauseSemantics;
};

struct NativeEHHandlerTarget {
  const llvm::BasicBlock *Target = nullptr;
  va_t SourceVA = 0;
};

struct NativeEHProtectedInvoke {
  const llvm::InvokeInst *Invoke = nullptr;
  va_t SourceVA = 0;
  uint32_t Region = 0;
};

struct NativeEHRangeMarker {
  const llvm::InvokeInst *Invoke = nullptr;
  va_t SourceVA = 0;
};

struct NativeEHRangeTarget {
  const llvm::BasicBlock *Target = nullptr;
  va_t SourceVA = 0;
};

bool hasExactCxxTypeDescriptor(const llvm::Function &Function,
                               const llvm::Value &Argument,
                               va_t TypeDescriptorVA) {
  if (TypeDescriptorVA == 0)
    return llvm::isa<llvm::ConstantPointerNull>(Argument);
  const auto *Descriptor = llvm::dyn_cast<llvm::GlobalVariable>(
      Argument.stripPointerCasts());
  if (!Descriptor ||
      Descriptor !=
          Function.getParent()->getNamedGlobal(
              makeNdDataSymbol(TypeDescriptorVA)) ||
      !Descriptor->isDeclaration() || !Descriptor->hasExternalLinkage() ||
      Descriptor->getVisibility() != llvm::GlobalValue::DefaultVisibility ||
      !Descriptor->isConstant() ||
      !Descriptor->getValueType()->isIntegerTy(8) ||
      Descriptor->getAddressSpace() != 0 || Descriptor->isThreadLocal())
    return false;
  return true;
}

bool hasExactSourceFunctionIdentity(const llvm::Function &Owner,
                                    const llvm::Value &Value, va_t SourceVA,
                                    const llvm::FunctionType &ExpectedType) {
  const auto *Function =
      llvm::dyn_cast<llvm::Function>(Value.stripPointerCasts());
  if (!Function || Function == &Owner ||
      Function->getFunctionType() != &ExpectedType ||
      Function->getCallingConv() != llvm::CallingConv::C ||
      !(Function->hasExternalLinkage() ||
        (Function->hasLocalLinkage() && !Function->isDeclaration())))
    return false;
  auto Identity = rewrite_source::getOriginalVA(*Function);
  if (!Identity) {
    llvm::consumeError(Identity.takeError());
    return false;
  }
  return *Identity && **Identity == SourceVA;
}

llvm::Error validateNativeLanguageIRGraph(const llvm::Function &F,
                                          const ExceptionFunction &EH,
                                          Arch TargetArch) {
  auto SourceIdentity = rewrite_source::getOriginalVA(F);
  if (!SourceIdentity)
    return patchError("invalid rewrite source identity on native WinEH "
                      "function " +
                      F.getName() + ": " +
                      llvm::toString(SourceIdentity.takeError()));
  if (!*SourceIdentity || **SourceIdentity != EH.CodeRange.Begin)
    return patchError("rewrite source identity does not match native WinEH "
                      "function " +
                      F.getName());
  const auto ExpectedModel =
      EH.Personality == ExceptionPersonality::CSpecificHandler
          ? windows_eh_md::NativeProvenanceModel::SEH
      : isCxxFH4SourcePersonality(EH.Personality)
          ? windows_eh_md::NativeProvenanceModel::CxxFH4
          : windows_eh_md::NativeProvenanceModel::CxxFH3;
  std::map<uint32_t, NativeEHDispatch> Dispatches;
  std::vector<NativeEHProtectedInvoke> ProtectedInvokes;
  std::set<const llvm::InvokeInst *> AnchoredInvokes;
  std::set<const llvm::InvokeInst *> FinallyCallbackInvokes;
  std::set<const llvm::InvokeInst *> ValidatedFinallyCallbackInvokes;
  std::set<va_t> SourceAddresses;
  std::map<std::pair<uint32_t, uint32_t>, NativeEHHandlerTarget> HandlerTargets;
  using RangeKey = std::pair<uint32_t, uint32_t>;
  std::map<RangeKey, NativeEHRangeMarker> RangeEnters;
  std::map<RangeKey, NativeEHRangeMarker> RangeExits;
  std::map<RangeKey, NativeEHRangeTarget> RangeEnterTargets;
  std::map<RangeKey, NativeEHRangeTarget> RangeExitTargets;
  std::set<const llvm::InvokeInst *> AnchoredRangeMarkers;

  for (const llvm::BasicBlock &Block : F) {
    for (const llvm::Instruction &Instruction : Block) {
      const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
      if (!Call)
        continue;
      unsigned ProvenanceCount =
          Call->countOperandBundlesOfType(windows_eh_md::ProvenanceBundle);
      if (ProvenanceCount == 0)
        continue;
      const auto *Anchor = llvm::dyn_cast<llvm::CallInst>(Call);
      if (ProvenanceCount != 1 || !Anchor)
        return patchError("malformed native WinEH provenance in function " +
                          F.getName());
      auto Provenance = parseNativeEHProvenance(*Anchor);
      if (!Provenance || Provenance->Model != ExpectedModel ||
          Provenance->FunctionVA != EH.CodeRange.Begin)
        return patchError("malformed native WinEH provenance in function " +
                          F.getName());

      if (Provenance->Role ==
          windows_eh_md::NativeProvenanceRole::ProtectedInvoke) {
        if (Anchor->getNumOperandBundles() != 1 || Provenance->Clause != 0 ||
            Provenance->AuxVA != 0 || Provenance->Flags != 0 ||
            !EH.CodeRange.contains(Provenance->SourceVA))
          return patchError(
              "malformed protected-invoke provenance in function " +
              F.getName());
        const auto *Invoke = llvm::dyn_cast_or_null<llvm::InvokeInst>(
            nextSemanticInstruction(*Anchor));
        const llvm::Function *Callee =
            Invoke ? Invoke->getCalledFunction() : nullptr;
        if (!Invoke || (Callee && Callee->isIntrinsic()) ||
            !AnchoredInvokes.insert(Invoke).second ||
            !SourceAddresses.insert(Provenance->SourceVA).second)
          return patchError(
              "protected invoke has invalid native WinEH provenance in "
              "function " +
              F.getName());
        ProtectedInvokes.push_back(
            {Invoke, Provenance->SourceVA, Provenance->Region});
        continue;
      }

      if (Provenance->Role ==
          windows_eh_md::NativeProvenanceRole::HandlerTarget) {
        const std::pair<uint32_t, uint32_t> Key{Provenance->Region,
                                                Provenance->Clause};
        if (Anchor->getNumOperandBundles() != 1 || Provenance->AuxVA != 0 ||
            Provenance->Flags != 0 ||
            !EH.CodeRange.contains(Provenance->SourceVA) ||
            !HandlerTargets
                 .emplace(Key, NativeEHHandlerTarget{Anchor->getParent(),
                                                     Provenance->SourceVA})
                 .second)
          return patchError("malformed native WinEH handler-target "
                            "provenance in function " +
                            F.getName());
        continue;
      }

      const bool IsRangeEnter =
          Provenance->Role == windows_eh_md::NativeProvenanceRole::RangeEnter;
      const bool IsRangeExit =
          Provenance->Role == windows_eh_md::NativeProvenanceRole::RangeExit;
      if (IsRangeEnter || IsRangeExit) {
        const RangeKey Key{Provenance->Region, Provenance->Clause};
        const auto ExpectedIntrinsic = IsRangeEnter
                                           ? llvm::Intrinsic::seh_try_begin
                                           : llvm::Intrinsic::seh_try_end;
        const auto *Invoke = llvm::dyn_cast_or_null<llvm::InvokeInst>(
            nextSemanticInstruction(*Anchor));
        const llvm::Function *Callee =
            Invoke ? Invoke->getCalledFunction() : nullptr;
        auto &Markers = IsRangeEnter ? RangeEnters : RangeExits;
        if (ExpectedModel != windows_eh_md::NativeProvenanceModel::SEH ||
            Anchor->getNumOperandBundles() != 1 || Provenance->AuxVA != 0 ||
            Provenance->Flags != 0 || !Invoke || !Callee ||
            Callee->getIntrinsicID() != ExpectedIntrinsic ||
            Invoke->arg_size() != 0 || Invoke->getNumOperandBundles() != 0 ||
            !AnchoredRangeMarkers.insert(Invoke).second ||
            !Markers
                 .emplace(Key,
                          NativeEHRangeMarker{Invoke, Provenance->SourceVA})
                 .second)
          return patchError("malformed native SEH range-marker provenance in "
                            "function " +
                            F.getName());
        continue;
      }

      const bool IsRangeEnterTarget =
          Provenance->Role ==
          windows_eh_md::NativeProvenanceRole::RangeEnterTarget;
      const bool IsRangeExitTarget =
          Provenance->Role ==
          windows_eh_md::NativeProvenanceRole::RangeExitTarget;
      if (IsRangeEnterTarget || IsRangeExitTarget) {
        const RangeKey Key{Provenance->Region, Provenance->Clause};
        auto &Targets =
            IsRangeEnterTarget ? RangeEnterTargets : RangeExitTargets;
        if (ExpectedModel != windows_eh_md::NativeProvenanceModel::SEH ||
            Anchor->getNumOperandBundles() != 1 || Provenance->AuxVA != 0 ||
            Provenance->Flags != 0 ||
            !Targets
                 .emplace(Key, NativeEHRangeTarget{Anchor->getParent(),
                                                   Provenance->SourceVA})
                 .second)
          return patchError("malformed native SEH range-target provenance in "
                            "function " +
                            F.getName());
        continue;
      }

      if (Anchor->getNumOperandBundles() != 2 ||
          Anchor->countOperandBundlesOfType("funclet") != 1)
        return patchError("malformed region-dispatch provenance in function " +
                          F.getName());
      auto FuncletBundle = Anchor->getOperandBundle("funclet");
      if (!FuncletBundle || FuncletBundle->Inputs.size() != 1)
        return patchError("malformed region-dispatch provenance in function " +
                          F.getName());

      const llvm::Instruction *Previous = previousSemanticInstruction(*Anchor);
      const llvm::BasicBlock *Destination = nullptr;
      bool IsCleanup = false;
      const llvm::Value *ExpectedToken = nullptr;
      const llvm::BasicBlock *Continuation = nullptr;
      const llvm::CatchPadInst *CatchPad = nullptr;
      const llvm::CleanupPadInst *CleanupPad = nullptr;
      if (const auto *Pad =
              llvm::dyn_cast_or_null<llvm::CatchPadInst>(Previous)) {
        CatchPad = Pad;
        Destination = Pad->getCatchSwitch()->getParent();
        ExpectedToken = Pad;
        const auto *Return = llvm::dyn_cast_or_null<llvm::CatchReturnInst>(
            nextSemanticInstruction(*Anchor));
        if (!Return || Return->getCatchPad() != Pad)
          return patchError("region-dispatch provenance has no exact catchret "
                            "in function " +
                            F.getName());
        Continuation = Return->getSuccessor();
      } else if (const auto *Pad =
                     llvm::dyn_cast_or_null<llvm::CleanupPadInst>(Previous)) {
        CleanupPad = Pad;
        Destination = Pad->getParent();
        ExpectedToken = Pad;
        IsCleanup = true;
      }
      if (!Destination || FuncletBundle->Inputs.front().get() != ExpectedToken)
        return patchError("region-dispatch provenance is detached from its "
                          "funclet in function " +
                          F.getName());

      NativeEHDispatch &Dispatch = Dispatches[Provenance->Region];
      if ((Dispatch.Destination && (Dispatch.Destination != Destination ||
                                    Dispatch.IsCleanup != IsCleanup)) ||
          !Dispatch.Clauses.insert(Provenance->Clause).second ||
          !Dispatch.ClauseSemantics
               .emplace(Provenance->Clause,
                        NativeEHClause{CatchPad, CleanupPad,
                                       Provenance->SourceVA, Provenance->AuxVA,
                                       Provenance->Flags})
               .second ||
          (Continuation &&
           !Dispatch.Continuations.emplace(Provenance->Clause, Continuation)
                .second))
        return patchError("ambiguous native WinEH region provenance in "
                          "function " +
                          F.getName());
      Dispatch.Destination = Destination;
      Dispatch.IsCleanup = IsCleanup;
    }
  }

  if (Dispatches.empty() ||
      (ExpectedModel != windows_eh_md::NativeProvenanceModel::SEH &&
       ProtectedInvokes.empty()))
    return patchError("native WinEH provenance is incomplete for function " +
                      F.getName());
  for (const auto &[Region, Dispatch] : Dispatches) {
    if (!Dispatch.IsCleanup)
      continue;
    const llvm::InvokeInst *Callback = nullptr;
    for (const llvm::Instruction &Instruction : *Dispatch.Destination) {
      const auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(&Instruction);
      if (!Invoke)
        continue;
      const llvm::Function *Callee = Invoke->getCalledFunction();
      if (Callee && Callee->isIntrinsic())
        continue;
      if (Callback)
        return patchError("native SEH finally dispatch has ambiguous callback "
                          "invokes in function " +
                          F.getName());
      Callback = Invoke;
    }
    if (Callback)
      FinallyCallbackInvokes.insert(Callback);
  }
  for (const llvm::BasicBlock &Block : F) {
    for (const llvm::Instruction &Instruction : Block) {
      const auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(&Instruction);
      if (!Invoke)
        continue;
      const llvm::Function *Callee = Invoke->getCalledFunction();
      if (Callee && Callee->isIntrinsic()) {
        if ((Callee->getIntrinsicID() == llvm::Intrinsic::seh_try_begin ||
             Callee->getIntrinsicID() == llvm::Intrinsic::seh_try_end) &&
            !AnchoredRangeMarkers.count(Invoke))
          return patchError("SEH range marker lacks native provenance in "
                            "function " +
                            F.getName());
        continue;
      }
      if ((!AnchoredInvokes.count(Invoke) &&
           !FinallyCallbackInvokes.count(Invoke)) ||
          previousSemanticInstruction(*Invoke) == nullptr)
        return patchError("protected invoke lacks native WinEH provenance in "
                          "function " +
                          F.getName());
    }
  }

  auto ValidateUnwindDestinations = [&]() -> llvm::Error {
    for (const NativeEHProtectedInvoke &Protected : ProtectedInvokes) {
      auto Dispatch = Dispatches.find(Protected.Region);
      if (Dispatch == Dispatches.end() ||
          Protected.Invoke->getUnwindDest() != Dispatch->second.Destination)
        return patchError("protected invoke unwind destination does not match "
                          "its source region in function " +
                          F.getName());
    }
    return llvm::Error::success();
  };

  if (ExpectedModel == windows_eh_md::NativeProvenanceModel::SEH) {
    if (!EH.SEH || Dispatches.size() != EH.SEH->Scopes.size())
      return patchError("native SEH region provenance is incomplete for "
                        "function " +
                        F.getName());
    auto ParentRegion = [&](size_t Region) -> std::optional<uint32_t> {
      const ExceptionAddressRange &Range = EH.SEH->Scopes[Region].GuardedRange;
      std::optional<uint32_t> Parent;
      uint64_t ParentSize = std::numeric_limits<uint64_t>::max();
      for (size_t Candidate = 0; Candidate < EH.SEH->Scopes.size();
           ++Candidate) {
        const ExceptionAddressRange &CandidateRange =
            EH.SEH->Scopes[Candidate].GuardedRange;
        if (Candidate == Region || !CandidateRange.contains(Range) ||
            CandidateRange.size() <= Range.size() ||
            CandidateRange.size() >= ParentSize)
          continue;
        Parent = static_cast<uint32_t>(Candidate);
        ParentSize = CandidateRange.size();
      }
      return Parent;
    };
    auto ExpectedParentDestination =
        [&](size_t Region) -> const llvm::BasicBlock * {
      std::optional<uint32_t> Parent = ParentRegion(Region);
      if (!Parent)
        return nullptr;
      auto Dispatch = Dispatches.find(*Parent);
      return Dispatch == Dispatches.end() ? nullptr
                                          : Dispatch->second.Destination;
    };
    auto *PtrTy = llvm::PointerType::get(F.getContext(), 0);
    auto *FilterType = llvm::FunctionType::get(
        llvm::Type::getInt32Ty(F.getContext()), {PtrTy, PtrTy}, false);
    auto *FinallyType = llvm::FunctionType::get(
        llvm::Type::getVoidTy(F.getContext()),
        {llvm::Type::getInt8Ty(F.getContext()), PtrTy}, false);
    auto ValidateRangeMarkers =
        [&](const std::map<RangeKey, NativeEHRangeMarker> &Markers,
            const std::map<RangeKey, NativeEHRangeTarget> &Targets,
            bool IsEnter) -> llvm::Error {
      if (Markers.size() != Targets.size())
        return patchError("native SEH range provenance has invalid exact "
                          "cardinality in function " +
                          F.getName());
      std::map<uint32_t, std::set<uint32_t>> Clauses;
      for (const auto &[Key, Marker] : Markers) {
        if (Key.first >= EH.SEH->Scopes.size())
          return patchError("native SEH range provenance names an unknown "
                            "scope in function " +
                            F.getName());
        auto Target = Targets.find(Key);
        auto Dispatch = Dispatches.find(Key.first);
        const ExceptionAddressRange &Range =
            EH.SEH->Scopes[Key.first].GuardedRange;
        const va_t ExpectedSource = IsEnter ? Range.Begin : Range.End;
        if (Target == Targets.end() || Dispatch == Dispatches.end() ||
            Marker.SourceVA != ExpectedSource ||
            Target->second.SourceVA != ExpectedSource || !Marker.Invoke ||
            Marker.Invoke->getNormalDest() != Target->second.Target ||
            Marker.Invoke->getUnwindDest() != Dispatch->second.Destination)
          return patchError("native SEH range marker has an altered "
                            "normal/unwind destination in function " +
                            F.getName());
        Clauses[Key.first].insert(Key.second);
      }
      for (const auto &[Key, Target] : Targets) {
        (void)Target;
        if (!Markers.count(Key))
          return patchError("native SEH range-target provenance has no exact "
                            "marker in function " +
                            F.getName());
      }
      for (size_t Region = 0; Region < EH.SEH->Scopes.size(); ++Region) {
        auto RegionClauses = Clauses.find(static_cast<uint32_t>(Region));
        if (IsEnter && RegionClauses == Clauses.end())
          return patchError("native SEH range-enter provenance is incomplete "
                            "for function " +
                            F.getName());
        if (RegionClauses == Clauses.end())
          continue;
        uint32_t ExpectedClause = 0;
        for (uint32_t Clause : RegionClauses->second)
          if (Clause != ExpectedClause++)
            return patchError("native SEH range provenance has non-canonical "
                              "cardinality in function " +
                              F.getName());
      }
      return llvm::Error::success();
    };
    if (llvm::Error Err =
            ValidateRangeMarkers(RangeEnters, RangeEnterTargets, true))
      return Err;
    if (llvm::Error Err =
            ValidateRangeMarkers(RangeExits, RangeExitTargets, false))
      return Err;
    size_t ExpectedHandlerTargets = 0;
    for (size_t I = 0; I < EH.SEH->Scopes.size(); ++I) {
      auto Dispatch = Dispatches.find(static_cast<uint32_t>(I));
      const SEHScopeRecord &Scope = EH.SEH->Scopes[I];
      if (Dispatch == Dispatches.end() ||
          Dispatch->second.Clauses.size() != 1 ||
          !Dispatch->second.Clauses.count(0) ||
          Dispatch->second.IsCleanup != (Scope.Kind == SEHScopeKind::Finally))
        return patchError("native SEH region provenance does not match the "
                          "input scope table for function " +
                          F.getName());
      auto Semantics = Dispatch->second.ClauseSemantics.find(0);
      if (Semantics == Dispatch->second.ClauseSemantics.end() ||
          Semantics->second.Flags != static_cast<uint32_t>(Scope.Kind))
        return patchError("native SEH funclet provenance does not match the "
                          "input scope table for function " +
                          F.getName());

      const llvm::BasicBlock *ParentDestination =
          ExpectedParentDestination(I);
      if (Scope.Kind != SEHScopeKind::Finally) {
        ++ExpectedHandlerTargets;
        const std::pair<uint32_t, uint32_t> Key{
            static_cast<uint32_t>(I), 0};
        auto HandlerTarget = HandlerTargets.find(Key);
        auto Continuation = Dispatch->second.Continuations.find(0);
        const va_t ExpectedFilter = Scope.Kind == SEHScopeKind::Filter
                                        ? Scope.FilterOrFinallyVA
                                        : 0;
        if (!Semantics->second.CatchPad || Semantics->second.CleanupPad ||
            Semantics->second.SourceVA != Scope.HandlerVA ||
            Semantics->second.AuxVA != ExpectedFilter ||
            HandlerTarget == HandlerTargets.end() ||
            HandlerTarget->second.SourceVA != Scope.HandlerVA ||
            Continuation == Dispatch->second.Continuations.end() ||
            Continuation->second != HandlerTarget->second.Target)
          return patchError(
              "native SEH catchret continuation does not match its source "
              "handler in function " +
              F.getName());

        const llvm::CatchPadInst &Pad = *Semantics->second.CatchPad;
        const auto ExpectedToken =
            windows_eh_semantics::getSEHScopeSemanticToken(
                EH, TargetArch, static_cast<uint32_t>(I));
        if (!ExpectedToken ||
            parseRewriteWinEHSemanticToken(
                Pad, llvm::mc_rewrite::RewriteWinEHSemanticKind::SEHScope) !=
                ExpectedToken)
          return patchError(
              "native SEH catchpad semantic token does not match its source "
              "scope in function " +
              F.getName());
        const llvm::CatchSwitchInst *Switch = Pad.getCatchSwitch();
        if (Pad.arg_size() != 1 || !Switch || Switch->getNumHandlers() != 1 ||
            *Switch->handler_begin() != Pad.getParent() ||
            !llvm::isa<llvm::ConstantTokenNone>(Switch->getParentPad()) ||
            Switch->getUnwindDest() != ParentDestination ||
            Switch->hasUnwindDest() != (ParentDestination != nullptr))
          return patchError("native SEH catch funclet ABI was altered in "
                            "function " +
                            F.getName());
        if (Scope.Kind == SEHScopeKind::CatchAll) {
          if (!llvm::isa<llvm::ConstantPointerNull>(Pad.getArgOperand(0)))
            return patchError("native SEH catch-all filter was altered in "
                              "function " +
                              F.getName());
        } else if (!hasExactSourceFunctionIdentity(F, *Pad.getArgOperand(0),
                                                   Scope.FilterOrFinallyVA,
                                                   *FilterType)) {
          return patchError("native SEH filter callback was altered in "
                            "function " +
                            F.getName());
        }
        continue;
      }

      if (Semantics->second.CatchPad || !Semantics->second.CleanupPad ||
          Semantics->second.SourceVA != 0 ||
          Semantics->second.AuxVA != Scope.FilterOrFinallyVA)
        return patchError("native SEH finally provenance does not match its "
                          "source callback in function " +
                          F.getName());
      const llvm::CleanupPadInst &Pad = *Semantics->second.CleanupPad;
      const auto ExpectedToken = windows_eh_semantics::getSEHScopeSemanticToken(
          EH, TargetArch, static_cast<uint32_t>(I));
      if (!ExpectedToken ||
          parseRewriteWinEHSemanticToken(
              Pad, llvm::mc_rewrite::RewriteWinEHSemanticKind::SEHScope) !=
              ExpectedToken)
        return patchError(
            "native SEH cleanuppad semantic token does not match its source "
            "scope in function " +
            F.getName());
      if (Pad.arg_size() != 0 ||
          !llvm::isa<llvm::ConstantTokenNone>(Pad.getParentPad()))
        return patchError("native SEH finally cleanupret was altered in "
                          "function " +
                          F.getName());

      const llvm::CallInst *LocalAddress = nullptr;
      const llvm::CallBase *CallbackCall = nullptr;
      size_t LocalAddressCount = 0;
      size_t CallbackCount = 0;
      for (const llvm::Instruction &Instruction : *Pad.getParent()) {
        const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
        if (!Call)
          continue;
        const llvm::Function *Callee = Call->getCalledFunction();
        if (Callee && Callee->getIntrinsicID() == llvm::Intrinsic::sideeffect)
          continue;
        if (Callee &&
            Callee->getIntrinsicID() == llvm::Intrinsic::localaddress) {
          LocalAddress = llvm::dyn_cast<llvm::CallInst>(Call);
          ++LocalAddressCount;
          continue;
        }
        CallbackCall = Call;
        ++CallbackCount;
      }
      const llvm::Function *CallbackFunction =
          CallbackCall
              ? llvm::dyn_cast<llvm::Function>(
                    CallbackCall->getCalledOperand()->stripPointerCasts())
              : nullptr;
      if (LocalAddressCount > 1 ||
          (LocalAddressCount == 1 &&
           (!LocalAddress || LocalAddress->arg_size() != 0 ||
            LocalAddress->getNumOperandBundles() != 0)) ||
          CallbackCount != 1 || !CallbackCall || !CallbackFunction ||
          CallbackCall->arg_size() != 2 ||
          CallbackCall->getFunctionType() != FinallyType ||
          CallbackCall->getCallingConv() != llvm::CallingConv::C ||
          CallbackCall->getNumOperandBundles() != 1 ||
          CallbackCall->countOperandBundlesOfType("funclet") != 1 ||
          !hasExactSourceFunctionIdentity(F, *CallbackCall->getCalledOperand(),
                                          Scope.FilterOrFinallyVA,
                                          *FinallyType))
        return patchError("native SEH finally callback was altered in "
                          "function " +
                          F.getName());

      const llvm::CleanupReturnInst *Return = nullptr;
      if (ParentDestination) {
        const auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(CallbackCall);
        const llvm::BasicBlock *Continue =
            Invoke ? Invoke->getNormalDest() : nullptr;
        if (!Invoke || Invoke->getUnwindDest() != ParentDestination ||
            !Continue || Continue->getSinglePredecessor() != Pad.getParent() ||
            Continue->size() != 1 ||
            !ValidatedFinallyCallbackInvokes.insert(Invoke).second)
          return patchError("nested native SEH finally callback has an altered "
                            "unwind edge in function " +
                            F.getName());
        Return =
            llvm::dyn_cast<llvm::CleanupReturnInst>(Continue->getTerminator());
      } else {
        if (!llvm::isa<llvm::CallInst>(CallbackCall))
          return patchError("outer native SEH finally callback unexpectedly "
                            "uses a local unwind edge in function " +
                            F.getName());
        Return = llvm::dyn_cast<llvm::CleanupReturnInst>(
            Pad.getParent()->getTerminator());
      }
      if (!Return || Return->getCleanupPad() != &Pad ||
          Return->getUnwindDest() != ParentDestination ||
          Return->hasUnwindDest() != (ParentDestination != nullptr))
        return patchError("native SEH finally cleanupret was altered in "
                          "function " +
                          F.getName());
      auto Funclet = CallbackCall->getOperandBundle("funclet");
      const auto *AbnormalTermination =
          llvm::dyn_cast<llvm::ConstantInt>(CallbackCall->getArgOperand(0));
      auto IsProvablyDiscardedArgument = [&](unsigned Index) {
        return !CallbackFunction->isDeclaration() &&
               CallbackFunction->arg_size() == 2 &&
               CallbackFunction->getArg(Index)->use_empty() &&
               llvm::isa<llvm::UndefValue>(CallbackCall->getArgOperand(Index));
      };
      const bool HasExactAbnormalTermination =
          AbnormalTermination && AbnormalTermination->getBitWidth() == 8 &&
          AbnormalTermination->getZExtValue() == 1;
      const bool HasExactFrame = LocalAddressCount == 1 && LocalAddress &&
                                 CallbackCall->getArgOperand(1) == LocalAddress;
      const bool HasDiscardedFrame =
          LocalAddressCount == 0 && IsProvablyDiscardedArgument(1);
      if (!Funclet || Funclet->Inputs.size() != 1 ||
          Funclet->Inputs.front().get() != &Pad ||
          (!HasExactAbnormalTermination && !IsProvablyDiscardedArgument(0)) ||
          (!HasExactFrame && !HasDiscardedFrame))
        return patchError("native SEH finally callback ABI was altered in "
                          "function " +
                          F.getName());
    }
    if (HandlerTargets.size() != ExpectedHandlerTargets)
      return patchError("native SEH handler-target provenance has invalid "
                        "cardinality in function " +
                        F.getName());
    if (ValidatedFinallyCallbackInvokes != FinallyCallbackInvokes)
      return patchError("native SEH finally callback invoke provenance is "
                        "incomplete in function " +
                        F.getName());
    for (const NativeEHProtectedInvoke &Protected : ProtectedInvokes) {
      std::optional<uint32_t> ExpectedRegion;
      uint64_t InnermostSize = std::numeric_limits<uint64_t>::max();
      for (size_t I = 0; I < EH.SEH->Scopes.size(); ++I) {
        const ExceptionAddressRange &Range = EH.SEH->Scopes[I].GuardedRange;
        if (!Range.contains(Protected.SourceVA) ||
            Range.size() >= InnermostSize)
          continue;
        ExpectedRegion = static_cast<uint32_t>(I);
        InnermostSize = Range.size();
      }
      if (!ExpectedRegion || Protected.Region != *ExpectedRegion)
        return patchError("protected invoke does not match its source SEH "
                          "scope in function " +
                          F.getName());
    }
    return ValidateUnwindDestinations();
  }

  if (!RangeEnters.empty() || !RangeExits.empty() ||
      !RangeEnterTargets.empty() || !RangeExitTargets.empty())
    return patchError("native C++ provenance contains SEH range markers in "
                      "function " +
                      F.getName());

  if (!EH.Cxx || Dispatches.size() != EH.Cxx->TryBlocks.size())
    return patchError("native C++ region provenance is incomplete for "
                      "function " +
                      F.getName());
  size_t ExpectedHandlerTargets = 0;
  for (size_t I = 0; I < EH.Cxx->TryBlocks.size(); ++I) {
    auto Dispatch = Dispatches.find(static_cast<uint32_t>(I));
    const CxxTryBlock &Try = EH.Cxx->TryBlocks[I];
    ExpectedHandlerTargets += Try.Handlers.size();
    if (Dispatch == Dispatches.end() || Dispatch->second.IsCleanup ||
        Dispatch->second.Clauses.size() != Try.Handlers.size())
      return patchError("native C++ region provenance does not match the input "
                        "try map for function " +
                        F.getName());
    for (size_t Clause = 0; Clause < Try.Handlers.size(); ++Clause) {
      const uint32_t ClauseIndex = static_cast<uint32_t>(Clause);
      const std::pair<uint32_t, uint32_t> Key{static_cast<uint32_t>(I),
                                              ClauseIndex};
      auto HandlerTarget = HandlerTargets.find(Key);
      auto Continuation = Dispatch->second.Continuations.find(ClauseIndex);
      auto Semantics = Dispatch->second.ClauseSemantics.find(ClauseIndex);
      if (!Dispatch->second.Clauses.count(ClauseIndex))
        return patchError("native C++ clause provenance does not match the "
                          "input try map for function " +
                          F.getName());
      if (HandlerTarget == HandlerTargets.end() ||
          HandlerTarget->second.SourceVA != Try.Handlers[Clause].HandlerVA ||
          Continuation == Dispatch->second.Continuations.end() ||
          Continuation->second != HandlerTarget->second.Target)
        return patchError("native C++ catchret continuation does not match its "
                          "source handler in function " +
                          F.getName());
      const CxxCatchHandler &Handler = Try.Handlers[Clause];
      if (Semantics == Dispatch->second.ClauseSemantics.end() ||
          !Semantics->second.CatchPad || Semantics->second.CleanupPad ||
          Semantics->second.SourceVA != Handler.HandlerVA ||
          Semantics->second.AuxVA != Handler.TypeDescriptorVA ||
          Semantics->second.Flags != Handler.Adjectives)
        return patchError("native C++ catchpad provenance does not match its "
                          "source handler in function " +
                          F.getName());
      const llvm::CatchPadInst &Pad = *Semantics->second.CatchPad;
      const auto ExpectedToken = windows_eh_semantics::getCxxCatchSemanticToken(
          EH, TargetArch, static_cast<uint32_t>(I), ClauseIndex);
      if (!ExpectedToken ||
          parseRewriteWinEHSemanticToken(
              Pad, llvm::mc_rewrite::RewriteWinEHSemanticKind::CxxCatch) !=
              ExpectedToken)
        return patchError(
            "native C++ catchpad semantic token does not match its source "
            "clause in function " +
            F.getName());
      if (Pad.arg_size() != 3 ||
          !llvm::isa<llvm::ConstantPointerNull>(Pad.getArgOperand(2)))
        return patchError("native C++ catchpad ABI was altered in function " +
                          F.getName());
      if (!hasExactCxxTypeDescriptor(F, *Pad.getArgOperand(0),
                                     Handler.TypeDescriptorVA))
        return patchError("native C++ catchpad RTTI was altered in function " +
                          F.getName());
      const auto *Adjectives =
          llvm::dyn_cast<llvm::ConstantInt>(Pad.getArgOperand(1));
      if (!Adjectives || Adjectives->getBitWidth() != 32 ||
          Adjectives->getZExtValue() != Handler.Adjectives)
        return patchError(
            "native C++ catchpad adjectives were altered in function " +
            F.getName());
    }
  }
  if (HandlerTargets.size() != ExpectedHandlerTargets)
    return patchError("native C++ handler-target provenance has invalid "
                      "cardinality in function " +
                      F.getName());
  for (const NativeEHProtectedInvoke &Protected : ProtectedInvokes) {
    int32_t State = -1;
    for (const CxxIPState &Entry : EH.Cxx->IPMap) {
      if (Entry.IP > Protected.SourceVA)
        break;
      State = Entry.State;
    }
    std::optional<uint32_t> ExpectedRegion;
    uint64_t InnermostSpan = std::numeric_limits<uint64_t>::max();
    for (size_t I = 0; I < EH.Cxx->TryBlocks.size(); ++I) {
      const CxxTryBlock &Try = EH.Cxx->TryBlocks[I];
      if (State < Try.TryLow || State > Try.TryHigh)
        continue;
      uint64_t Span = static_cast<uint64_t>(Try.TryHigh) -
                      static_cast<uint64_t>(Try.TryLow);
      if (Span >= InnermostSpan)
        continue;
      ExpectedRegion = static_cast<uint32_t>(I);
      InnermostSpan = Span;
    }
    if (!ExpectedRegion || Protected.Region != *ExpectedRegion)
      return patchError("protected invoke does not match its source C++ try "
                        "region in function " +
                        F.getName());
  }
  return ValidateUnwindDestinations();
}

llvm::Error validateExceptionRecord(const ExceptionFunction &EH,
                                    Arch TargetArch,
                                    const llvm::Twine &Context) {
  if (EH.Kind != RuntimeFunctionKind::Primary)
    return patchError("function is not backed by a primary runtime record: " +
                      Context);
  if (EH.ParseStatus != ExceptionParseStatus::Complete)
    return patchError("incomplete exception metadata for " + Context);
  if (TargetArch == Arch::X64) {
    if (EH.Encoding != ExceptionEncoding::X64UnwindV1 &&
        EH.Encoding != ExceptionEncoding::X64UnwindV2)
      return patchError("unsupported unwind encoding " +
                        llvm::Twine(getExceptionEncodingName(EH.Encoding)) +
                        " in " + Context);
  } else if (TargetArch == Arch::AArch64) {
    if (EH.Encoding != ExceptionEncoding::ARM64Packed &&
        EH.Encoding != ExceptionEncoding::ARM64Unpacked)
      return patchError("unsupported ARM64 unwind encoding in " + Context);
  } else if (TargetArch == Arch::ARM) {
    if (EH.Encoding != ExceptionEncoding::ARM32Packed &&
        EH.Encoding != ExceptionEncoding::ARM32Unpacked)
      return patchError("unsupported ARM32 unwind encoding in " + Context);
  } else {
    return patchError("table-based exception regeneration is unsupported for " +
                      Context);
  }
  if (isGSWrappedPersonality(EH.Personality) &&
      EH.Personality != ExceptionPersonality::GSHandlerCheckEH4)
    return patchError("GS-wrapped language metadata cannot be regenerated "
                      "exactly for " +
                      Context);
  if (EH.Personality == ExceptionPersonality::Unknown)
    return patchError("unknown Windows personality in " + Context);

  if (EH.Personality != ExceptionPersonality::None) {
    const WindowsEHNativeSourceClassification Source =
        classifyWindowsEHNativeSource(EH, TargetArch, BinaryFormat::COFF,
                                      WindowsEHNativeCapability::OutputPatch);
    const bool ModelMatchesPersonality =
        (EH.Personality == ExceptionPersonality::CSpecificHandler &&
         Source.Model == WindowsEHNativeSourceModel::SEH) ||
        (EH.Personality == ExceptionPersonality::CxxFrameHandler3 &&
         Source.Model == WindowsEHNativeSourceModel::CxxFH3) ||
        (isCxxFH4SourcePersonality(EH.Personality) &&
         Source.Model == WindowsEHNativeSourceModel::CxxFH4);
    if (!Source.canPatchOutput() || !ModelMatchesPersonality)
      return patchError(
          "language metadata failed target-aware regeneration checks for " +
          Context + ": " + getWindowsEHNativeSourceReasonName(Source.Reason));
  }
  return llvm::Error::success();
}

llvm::Error validateExceptionFunction(const llvm::Function &F,
                                      const ExceptionFunction &EH,
                                      Arch TargetArch) {
  if (llvm::Error Err = validateExceptionRecord(
          EH, TargetArch, llvm::Twine("function ") + F.getName()))
    return Err;

  if (EH.Personality != ExceptionPersonality::None) {
    const bool IsGSWrapped =
        EH.Personality == ExceptionPersonality::GSHandlerCheckEH4;
    const bool IsFH4 = isCxxFH4SourcePersonality(EH.Personality);
    llvm::StringRef NativeKind =
        EH.Personality == ExceptionPersonality::CSpecificHandler
            ? (TargetArch == Arch::AArch64 ? "seh-aarch64-native"
               : TargetArch == Arch::ARM   ? "seh-arm32-native"
                                           : "seh-x64-native")
        : IsFH4 ? "cxx-fh4-native"
                : "cxx-fh3-native";
    llvm::StringRef PersonalityName =
        EH.Personality == ExceptionPersonality::CSpecificHandler
            ? "__C_specific_handler"
        : IsFH4 ? "__CxxFrameHandler4"
                : "__CxxFrameHandler3";
    if (!hasNativeEHMarker(F, NativeKind))
      return patchError("native WinEH lowering is unavailable for function " +
                        F.getName());
    if (!hasPersonality(F, PersonalityName))
      return patchError("native WinEH IR contract was altered for function " +
                        F.getName());
    const auto *BasePersonality = llvm::dyn_cast<llvm::Function>(
        F.getPersonalityFn()->stripPointerCasts());
    if (!isExactExternalWinEHDeclaration(BasePersonality, PersonalityName))
      return patchError(
          "native WinEH personality declaration was altered for function " +
          F.getName());
    if (F.hasFnAttribute(llvm::mc_rewrite::RewriteWinCxxFH4Attribute) != IsFH4)
      return patchError(
          "native C++ EH writer contract was altered for function " +
          F.getName());
    const llvm::Attribute GSWriter =
        F.getFnAttribute(llvm::mc_rewrite::RewriteWinGSHandlerAttribute);
    const bool HasExactGSWriter =
        GSWriter.isStringAttribute() &&
        GSWriter.getValueAsString() ==
            llvm::mc_rewrite::RewriteWinGSHandlerCxxFH4;
    if (F.hasFnAttribute(llvm::mc_rewrite::RewriteWinGSHandlerAttribute) !=
            IsGSWrapped ||
        HasExactGSWriter != IsGSWrapped ||
        F.hasFnAttribute(llvm::Attribute::StackProtectReq) != IsGSWrapped ||
        (IsGSWrapped &&
         (F.hasFnAttribute(llvm::Attribute::StackProtect) ||
          F.hasFnAttribute(llvm::Attribute::StackProtectStrong))))
      return patchError(
          "native C++ GS writer contract was altered for function " +
          F.getName());
    if (IsGSWrapped) {
      const llvm::Function *Wrapper =
          F.getParent()->getFunction("__GSHandlerCheck_EH4");
      if (!isExactExternalWinEHDeclaration(Wrapper, "__GSHandlerCheck_EH4") ||
          Wrapper == BasePersonality || Wrapper == &F ||
          Wrapper->getFunctionType() != BasePersonality->getFunctionType() ||
          Wrapper->getAddressSpace() != BasePersonality->getAddressSpace())
        return patchError(
            "native C++ GS wrapper declaration was altered for function " +
            F.getName());
    }
    if (llvm::Error Err = validateNativeLanguageIRGraph(F, EH, TargetArch))
      return Err;
  }
  return llvm::Error::success();
}

std::optional<size_t> rvaToFileOffset(const PEHeaderPtrs &PE,
                                      llvm::ArrayRef<uint8_t> Binary,
                                      uint32_t RVA, uint64_t Size) {
  uint32_t HeaderSize = getPESizeOfHeaders(PE);
  if (RVA < HeaderSize) {
    if (Size <= HeaderSize - RVA && rangeInBounds(RVA, Size, Binary.size()))
      return static_cast<size_t>(RVA);
    return std::nullopt;
  }

  std::optional<size_t> Result;
  forEachPESection(PE, [&](const PESectionFields &S, uint16_t) {
    if (Result || RVA < S.VirtualAddress)
      return;
    uint64_t Delta = uint64_t(RVA) - S.VirtualAddress;
    if (Delta > S.SizeOfRawData || Size > S.SizeOfRawData - Delta)
      return;
    uint64_t FileOffset = uint64_t(S.PointerToRawData) + Delta;
    if (!rangeInBounds(FileOffset, Size, Binary.size()))
      return;
    Result = static_cast<size_t>(FileOffset);
  });
  return Result;
}

struct RuntimeEntry {
  uint32_t Begin = 0;
  uint32_t End = 0;
  uint32_t Unwind = 0;
  uint32_t Handler = 0;
  uint32_t HandlerData = 0;
  bool HasLanguageHandler = false;
  std::array<uint32_t, 3> Words{};
  uint8_t WordCount = 0;

  friend bool operator==(const RuntimeEntry &A, const RuntimeEntry &B) {
    return A.WordCount == B.WordCount && A.Words == B.Words;
  }
};

template <typename ReadRVA>
llvm::Expected<RuntimeEntry> decodeRuntimeEntry(const uint8_t *Record,
                                                Arch TargetArch,
                                                ReadRVA &&ReadAtRVA) {
  RuntimeEntry Entry;
  if (TargetArch == Arch::X64) {
    Entry.Words = {readLE<uint32_t>(Record), readLE<uint32_t>(Record + 4),
                   readLE<uint32_t>(Record + 8)};
    Entry.WordCount = 3;
    Entry.Begin = Entry.Words[0];
    Entry.End = Entry.Words[1];
    Entry.Unwind = Entry.Words[2];

    const uint8_t *Header = ReadAtRVA(Entry.Unwind, 4);
    if (!Header)
      return patchError("x64 runtime-function entry references missing unwind "
                        "information");
    const uint8_t Version = Header[0] & 0x7u;
    const uint8_t Flags = Header[0] >> 3;
    const uint8_t HandlerFlags = llvm::Win64EH::UNW_ExceptionHandler |
                                 llvm::Win64EH::UNW_TerminateHandler;
    if ((Flags & HandlerFlags) != 0 &&
        (Flags & llvm::Win64EH::UNW_ChainInfo) != 0)
      return patchError("x64 unwind record combines handler and chain flags");
    if ((Version == 1 || Version == 2) && (Flags & HandlerFlags) != 0) {
      const uint64_t SlotCount = (uint64_t(Header[2]) + 1) & ~uint64_t(1);
      const uint64_t HandlerOffset = 4 + SlotCount * 2;
      if (HandlerOffset > std::numeric_limits<uint32_t>::max() - Entry.Unwind)
        return patchError("x64 language-handler location overflows");
      if (HandlerOffset + sizeof(uint32_t) >
          std::numeric_limits<uint32_t>::max() - Entry.Unwind)
        return patchError("x64 handler-data location overflows");
      const uint8_t *Handler =
          ReadAtRVA(Entry.Unwind + static_cast<uint32_t>(HandlerOffset),
                    sizeof(uint32_t));
      if (!Handler)
        return patchError("x64 language-handler field is truncated");
      Entry.Handler = readLE<uint32_t>(Handler);
      if (Entry.Handler == 0)
        return patchError("x64 unwind record has a zero language handler");
      Entry.HandlerData = Entry.Unwind + static_cast<uint32_t>(HandlerOffset) +
                          sizeof(uint32_t);
      Entry.HasLanguageHandler = true;
    }
    return Entry;
  }
  if (TargetArch != Arch::AArch64 && TargetArch != Arch::ARM)
    return patchError("unsupported runtime-function architecture");

  Entry.Words = {readLE<uint32_t>(Record), readLE<uint32_t>(Record + 4), 0};
  Entry.WordCount = 2;
  Entry.Begin =
      TargetArch == Arch::ARM ? Entry.Words[0] & ~uint32_t(1) : Entry.Words[0];
  Entry.Unwind = Entry.Words[1];

  llvm::support::ulittle32_t NativeWords[2];
  NativeWords[0] = Entry.Words[0];
  NativeWords[1] = Entry.Words[1];
  llvm::ARM::WinEH::RuntimeFunctionFlag Flag;
  uint32_t Length = 0;
  uint32_t XDataRVA = 0;
  if (TargetArch == Arch::AArch64) {
    llvm::ARM::WinEH::RuntimeFunctionARM64 RF(NativeWords);
    Flag = RF.Flag();
    if (Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_Packed ||
        Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_PackedFragment)
      Length = RF.FunctionLength();
    else if (Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_Unpacked)
      XDataRVA = RF.ExceptionInformationRVA();
  } else {
    llvm::ARM::WinEH::RuntimeFunction RF(NativeWords);
    Flag = RF.Flag();
    if (Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_Packed ||
        Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_PackedFragment)
      Length = RF.FunctionLength();
    else if (Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_Unpacked)
      XDataRVA = RF.ExceptionInformationRVA();
  }
  if (Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_Reserved)
    return patchError("ARM runtime-function entry uses reserved flags");

  if (Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_Unpacked) {
    const uint8_t *Header = ReadAtRVA(XDataRVA, sizeof(uint32_t));
    if (!Header)
      return patchError("ARM runtime-function entry references missing xdata");
    uint32_t First = readLE<uint32_t>(Header);
    bool Extended = TargetArch == Arch::AArch64 ? (First & 0xffc00000u) == 0
                                                : (First & 0xff800000u) == 0;
    size_t HeaderBytes = Extended ? 8 : 4;
    Header = ReadAtRVA(XDataRVA, HeaderBytes);
    if (!Header)
      return patchError("ARM runtime-function entry has truncated xdata");
    llvm::support::ulittle32_t XDataWords[2] = {};
    std::memcpy(XDataWords, Header, HeaderBytes);
    llvm::ARM::WinEH::ExceptionDataRecord XR(XDataWords,
                                             TargetArch == Arch::AArch64);
    if (XR.Vers() != 0)
      return patchError("ARM xdata uses an unsupported version");
    Length = TargetArch == Arch::AArch64 ? XR.FunctionLengthInBytesAArch64()
                                         : XR.FunctionLengthInBytesARM();

    const uint64_t HeaderWords = Extended ? 2 : 1;
    const uint64_t EpilogueWords = XR.E() ? 0 : XR.EpilogueCount();
    const uint64_t CodeWords = XR.CodeWords();
    if (HeaderWords > std::numeric_limits<uint64_t>::max() - EpilogueWords ||
        HeaderWords + EpilogueWords >
            std::numeric_limits<uint64_t>::max() - CodeWords)
      return patchError("ARM xdata structural size overflows");
    const uint64_t PreHandlerWords = HeaderWords + EpilogueWords + CodeWords;
    const uint64_t StructuralWords = PreHandlerWords + (XR.X() ? 1 : 0);
    if (StructuralWords > std::numeric_limits<size_t>::max() / 4)
      return patchError("ARM xdata structural size exceeds host limits");
    const size_t StructuralBytes = static_cast<size_t>(StructuralWords * 4);
    const uint8_t *Body = ReadAtRVA(XDataRVA, StructuralBytes);
    if (!Body)
      return patchError("ARM xdata body is truncated");
    if (XR.X()) {
      Entry.Handler =
          readLE<uint32_t>(Body + static_cast<size_t>(PreHandlerWords * 4));
      if (Entry.Handler == 0)
        return patchError("ARM xdata has a zero language-handler RVA");
      if (TargetArch == Arch::ARM) {
        if ((Entry.Handler & 1u) == 0)
          return patchError(
              "ARM32 language handler does not encode a Thumb address");
        Entry.Handler &= ~uint32_t(1);
      }
      if (StructuralBytes > std::numeric_limits<uint32_t>::max() ||
          XDataRVA > std::numeric_limits<uint32_t>::max() - StructuralBytes)
        return patchError("ARM handler-data location overflows");
      Entry.HandlerData =
          XDataRVA + static_cast<uint32_t>(StructuralBytes);
      Entry.HasLanguageHandler = true;
    }
  }
  if (Length == 0 ||
      Length > std::numeric_limits<uint32_t>::max() - Entry.Begin)
    return patchError("ARM runtime-function entry has an invalid length");
  Entry.End = Entry.Begin + Length;
  return Entry;
}

template <typename ReadRVA>
llvm::Error validateGeneratedARMCatchAllTable(const RuntimeEntry &Entry,
                                              Arch TargetArch,
                                              uint32_t XDataEndRVA,
                                              ReadRVA &&ReadAtRVA) {
  const llvm::StringRef Architecture =
      TargetArch == Arch::ARM ? "ARM32" : "ARM64";
  if (!Entry.HasLanguageHandler || Entry.HandlerData == 0)
    return patchError("generated " + Architecture +
                      " SEH owner has no language-handler data");
  const uint8_t *CountBytes =
      ReadAtRVA(Entry.HandlerData, sizeof(uint32_t));
  if (!CountBytes)
    return patchError("generated " + Architecture +
                      " SEH scope count is truncated");
  const uint32_t Count = readLE<uint32_t>(CountBytes);
  if (Count == 0 || Count > MaxGeneratedLanguageRecords)
    return patchError("generated " + Architecture +
                      " SEH scope count is invalid");

  constexpr uint64_t ScopeRecordSize = 4 * sizeof(uint32_t);
  const uint64_t ScopeBytes = uint64_t(Count) * ScopeRecordSize;
  if (ScopeBytes > std::numeric_limits<size_t>::max() ||
      Entry.HandlerData >
          std::numeric_limits<uint32_t>::max() - sizeof(uint32_t))
    return patchError("generated " + Architecture +
                      " SEH scope table size overflows");
  const uint32_t ScopeTableRVA = Entry.HandlerData + sizeof(uint32_t);
  const uint64_t ScopeTableEnd = uint64_t(ScopeTableRVA) + ScopeBytes;
  if (ScopeTableEnd > XDataEndRVA)
    return patchError("generated " + Architecture +
                      " SEH scope table leaves its xdata section");
  const uint8_t *Scopes =
      ReadAtRVA(ScopeTableRVA, static_cast<size_t>(ScopeBytes));
  if (!Scopes)
    return patchError("generated " + Architecture +
                      " SEH scope table is truncated");

  for (uint32_t I = 0; I < Count; ++I) {
    const uint8_t *Scope = Scopes + size_t(I) * ScopeRecordSize;
    const uint32_t RawBegin = readLE<uint32_t>(Scope);
    const uint32_t RawEnd = readLE<uint32_t>(Scope + sizeof(uint32_t));
    const uint32_t FilterOrFinally =
        readLE<uint32_t>(Scope + 2 * sizeof(uint32_t));
    const uint32_t RawHandler =
        readLE<uint32_t>(Scope + 3 * sizeof(uint32_t));
    uint32_t Begin = RawBegin;
    uint32_t End = RawEnd;
    uint32_t Handler = RawHandler;
    if (TargetArch == Arch::ARM) {
      if (((RawBegin & RawEnd & RawHandler) & 1u) == 0)
        return patchError(
            "generated ARM32 SEH scope does not encode Thumb addresses");
      Begin &= ~uint32_t(1);
      End &= ~uint32_t(1);
      Handler &= ~uint32_t(1);
      if (((Begin | End | Handler) & 1u) != 0)
        return patchError("generated ARM32 SEH scope address is unaligned");
    } else if (((Begin | End | Handler) & 3u) != 0) {
      return patchError("generated ARM64 SEH scope address is unaligned");
    }
    if (Begin < Entry.Begin || Begin >= End || End > Entry.End)
      return patchError("generated " + Architecture +
                        " SEH guarded range leaves its exact owner");
    if (FilterOrFinally != 1)
      return patchError("generated " + Architecture +
                        " SEH table contains a non-catch-all scope");
    if (Handler < Entry.Begin || Handler >= Entry.End ||
        (Begin <= Handler && Handler < End))
      return patchError("generated " + Architecture +
                        " SEH handler placement is invalid");
  }
  return llvm::Error::success();
}

struct COFFCxxSourceGroup {
  exception_rewrite::CxxSourceGroup Source;
  std::vector<ExceptionAddressRange> MemberRanges;
};

llvm::Expected<std::vector<COFFCxxSourceGroup>>
buildCxxSourceGroupInventory(const BinaryImage &Image) {
  std::map<va_t, std::vector<const ExceptionFunction *>> RecordsByIdentity;
  for (const ExceptionFunction &EH : Image.ExceptionMetadata.Functions) {
    if (!EH.Cxx)
      continue;
    const CxxExceptionInfo &Cxx = *EH.Cxx;
    if (Cxx.NativeFuncInfoVA == InvalidVA)
      return patchError(
          "C++ EH contribution has an invalid native FuncInfo identity");
    if (Cxx.IsSeparated && Cxx.NativeFuncInfoVA == 0)
      return patchError(
          "separated C++ EH contribution lacks a valid native FuncInfo "
          "group identity");
    if (Cxx.NativeFuncInfoVA != 0 && Cxx.NativeFuncInfoVA != InvalidVA)
      RecordsByIdentity[Cxx.NativeFuncInfoVA].push_back(&EH);
  }

  std::vector<COFFCxxSourceGroup> Groups;
  std::vector<std::pair<ExceptionAddressRange, va_t>> GlobalRanges;
  for (const auto &[Identity, Records] : RecordsByIdentity) {
    const bool IsGroup =
        Records.size() > 1 ||
        std::any_of(Records.begin(), Records.end(),
                    [](const auto *EH) { return EH->Cxx->IsSeparated; });
    if (!IsGroup)
      continue;

    COFFCxxSourceGroup Group;
    Group.Source.GroupIdentity = Identity;
    size_t ParentCount = 0;
    for (const ExceptionFunction *EH : Records) {
      const CxxExceptionInfo &Cxx = *EH->Cxx;
      if (EH->Kind != RuntimeFunctionKind::Primary)
        return patchError("C++ FuncInfo group 0x" + llvm::utohexstr(Identity) +
                          " contains a non-primary runtime-function record");
      if (!Cxx.IsSeparated)
        return patchError("shared C++ FuncInfo group 0x" +
                          llvm::utohexstr(Identity) +
                          " has inconsistent separated-member flags");
      if (!EH->CodeRange.isValid() || EH->CodeRange.Begin == 0 ||
          EH->CodeRange.Begin == InvalidVA)
        return patchError("C++ FuncInfo group 0x" + llvm::utohexstr(Identity) +
                          " contains an invalid primary member entry");
      Group.Source.MemberVAs.push_back(EH->CodeRange.Begin);
      Group.MemberRanges.push_back(EH->CodeRange);
      GlobalRanges.push_back({EH->CodeRange, Identity});
      if (!Cxx.IsCatchFunclet) {
        ++ParentCount;
        Group.Source.CanonicalSourceOwnerVA = EH->CodeRange.Begin;
      }
    }
    if (ParentCount != 1)
      return patchError("C++ FuncInfo group 0x" + llvm::utohexstr(Identity) +
                        " does not have exactly one canonical parent");

    std::vector<size_t> Order(Group.Source.MemberVAs.size());
    for (size_t I = 0; I < Order.size(); ++I)
      Order[I] = I;
    std::sort(Order.begin(), Order.end(), [&](size_t A, size_t B) {
      return Group.Source.MemberVAs[A] < Group.Source.MemberVAs[B];
    });
    std::vector<uint64_t> SortedMembers;
    std::vector<ExceptionAddressRange> SortedRanges;
    SortedMembers.reserve(Order.size());
    SortedRanges.reserve(Order.size());
    for (size_t Index : Order) {
      if (!SortedMembers.empty() &&
          Group.Source.MemberVAs[Index] == SortedMembers.back())
        return patchError("C++ FuncInfo group 0x" + llvm::utohexstr(Identity) +
                          " contains a duplicate primary member entry");
      if (!SortedRanges.empty() &&
          SortedRanges.back().overlaps(Group.MemberRanges[Index]))
        return patchError("C++ FuncInfo group 0x" + llvm::utohexstr(Identity) +
                          " contains overlapping primary members");
      SortedMembers.push_back(Group.Source.MemberVAs[Index]);
      SortedRanges.push_back(Group.MemberRanges[Index]);
    }
    Group.Source.MemberVAs = std::move(SortedMembers);
    Group.MemberRanges = std::move(SortedRanges);
    Groups.push_back(std::move(Group));
  }

  std::sort(GlobalRanges.begin(), GlobalRanges.end(),
            [](const auto &A, const auto &B) {
              return std::tie(A.first.Begin, A.first.End, A.second) <
                     std::tie(B.first.Begin, B.first.End, B.second);
            });
  for (size_t I = 1; I < GlobalRanges.size(); ++I)
    if (GlobalRanges[I - 1].first.overlaps(GlobalRanges[I].first))
      return patchError("C++ FuncInfo group inventories overlap at primary "
                        "member entry 0x" +
                        llvm::utohexstr(GlobalRanges[I].first.Begin));
  return Groups;
}

llvm::Expected<std::vector<exception_rewrite::CxxSourceGroup>>
selectReplacedCxxSourceGroups(llvm::ArrayRef<COFFCxxSourceGroup> Inventory,
                              llvm::ArrayRef<va_t> ReplacedEntries) {
  std::set<va_t> Replaced;
  for (va_t Entry : ReplacedEntries)
    if (!Replaced.insert(Entry).second)
      return patchError("replacement entries contain a duplicate source "
                        "identity 0x" +
                        llvm::utohexstr(Entry));

  std::vector<exception_rewrite::CxxSourceGroup> Result;
  for (const COFFCxxSourceGroup &Group : Inventory) {
    size_t ReplacedCount = 0;
    for (va_t Member : Group.Source.MemberVAs)
      ReplacedCount += Replaced.count(Member);
    if (ReplacedCount == 0)
      continue;
    if (ReplacedCount != Group.Source.MemberVAs.size())
      return patchError("C++ FuncInfo group 0x" +
                        llvm::utohexstr(Group.Source.GroupIdentity) +
                        " cannot mix replaced and preserved source members");
    Result.push_back(Group.Source);
  }
  return Result;
}

std::optional<uint64_t> exactMetadataUInt(const llvm::MDNode &Node,
                                          unsigned Index, unsigned Width) {
  if (Index >= Node.getNumOperands())
    return std::nullopt;
  const auto *CAM = llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(
      Node.getOperand(Index).get());
  const auto *CI =
      CAM ? llvm::dyn_cast<llvm::ConstantInt>(CAM->getValue()) : nullptr;
  if (!CI || CI->getBitWidth() != Width)
    return std::nullopt;
  return CI->getZExtValue();
}

llvm::Expected<std::vector<exception_rewrite::CxxGroupRewriteContract>>
validateAvailableCxxGroupContracts(
    const llvm::Module &Mod, llvm::ArrayRef<COFFCxxSourceGroup> Inventory) {
  const llvm::NamedMDNode *Table =
      Mod.getNamedMetadata(exception_rewrite::CxxGroupTableMetadata);
  if (!Table)
    return std::vector<exception_rewrite::CxxGroupRewriteContract>{};

  std::vector<exception_rewrite::CxxSourceGroup> ContractSources;
  ContractSources.reserve(Table->getNumOperands());
  for (const llvm::MDNode *Row : Table->operands()) {
    if (!Row ||
        Row->getNumOperands() != exception_rewrite::CxxGroupOperandCount)
      return patchError("C++ group contract has an invalid identity row");
    const std::optional<uint64_t> Identity =
        exactMetadataUInt(*Row, exception_rewrite::CxxGroupIdentity, 64);
    if (!Identity || *Identity == 0 || *Identity == InvalidVA)
      return patchError("C++ group contract has an invalid identity");
    const auto Source = llvm::find_if(Inventory, [&](const auto &Candidate) {
      return Candidate.Source.GroupIdentity == *Identity;
    });
    if (Source == Inventory.end())
      return patchError("C++ group contract 0x" + llvm::utohexstr(*Identity) +
                        " has no loader-authenticated source group");
    ContractSources.push_back(Source->Source);
  }
  return exception_rewrite::validateCxxGroupRewriteContracts(Mod,
                                                             ContractSources);
}

llvm::Expected<std::vector<exception_rewrite::CxxGroupRewriteContract>>
validateReplacedCxxGroupContracts(const llvm::Module &Mod,
                                  llvm::ArrayRef<COFFCxxSourceGroup> Inventory,
                                  llvm::ArrayRef<va_t> ReplacedEntries,
                                  bool RequireExactContractSet = false) {
  auto ReplacedGroups =
      selectReplacedCxxSourceGroups(Inventory, ReplacedEntries);
  if (!ReplacedGroups)
    return ReplacedGroups.takeError();
  auto Available = validateAvailableCxxGroupContracts(Mod, Inventory);
  if (!Available)
    return Available.takeError();

  std::vector<exception_rewrite::CxxGroupRewriteContract> Result;
  Result.reserve(ReplacedGroups->size());
  for (const exception_rewrite::CxxSourceGroup &Source : *ReplacedGroups) {
    const auto Contract = llvm::find_if(*Available, [&](const auto &Candidate) {
      return Candidate.GroupIdentity == Source.GroupIdentity;
    });
    if (Contract == Available->end())
      return llvm::make_error<exception_rewrite::CxxGroupRewriteContractError>(
          exception_rewrite::CxxGroupContractErrorReason::MembershipMismatch,
          Source.GroupIdentity, 0,
          "replaced source group has no atomic rewrite contract");
    Result.push_back(*Contract);
  }
  if (RequireExactContractSet && Available->size() != Result.size())
    return patchError(
        "C++ group contract set is not exact for planned replacements");
  return Result;
}

std::set<uint32_t>
collectSupersededRuntimeRecords(const BinaryImage &Image,
                                llvm::ArrayRef<va_t> PatchedEntries,
                                std::vector<ExceptionAddressRange> &Ranges) {
  const auto &Functions = Image.ExceptionMetadata.Functions;
  std::set<size_t> Indices;
  for (va_t Entry : PatchedEntries)
    for (size_t I = 0; I < Functions.size(); ++I)
      if (Functions[I].CodeRange.contains(Entry))
        Indices.insert(I);

  bool Changed = true;
  while (Changed) {
    Changed = false;
    std::vector<size_t> Snapshot(Indices.begin(), Indices.end());
    for (size_t I : Snapshot) {
      const ExceptionFunction &F = Functions[I];
      if (F.PrimaryFunctionIndex && *F.PrimaryFunctionIndex < Functions.size())
        Changed |= Indices.insert(*F.PrimaryFunctionIndex).second;
      for (size_t J = 0; J < Functions.size(); ++J) {
        const ExceptionFunction &Candidate = Functions[J];
        if (Candidate.PrimaryFunctionIndex &&
            *Candidate.PrimaryFunctionIndex == I)
          Changed |= Indices.insert(J).second;
        if (Candidate.ChainedPrimaryRange &&
            Candidate.ChainedPrimaryRange->Begin == F.CodeRange.Begin &&
            Candidate.ChainedPrimaryRange->End == F.CodeRange.End)
          Changed |= Indices.insert(J).second;
      }
    }
  }

  std::set<uint32_t> RuntimeRVAs;
  for (size_t I : Indices) {
    const ExceptionFunction &F = Functions[I];
    Ranges.push_back(F.CodeRange);
    if (F.RuntimeFunctionRVA != 0)
      RuntimeRVAs.insert(F.RuntimeFunctionRVA);
  }
  return RuntimeRVAs;
}

bool overlapsAny(const RuntimeEntry &Entry, uint64_t ImageBase,
                 llvm::ArrayRef<ExceptionAddressRange> Ranges) {
  if (Entry.End <= Entry.Begin || Entry.Begin > InvalidVA - ImageBase ||
      Entry.End > InvalidVA - ImageBase)
    return false;
  ExceptionAddressRange Native{ImageBase + Entry.Begin, ImageBase + Entry.End};
  return std::any_of(Ranges.begin(), Ranges.end(),
                     [&](const ExceptionAddressRange &Range) {
                       return Native.overlaps(Range);
                     });
}

llvm::Error validateRuntimeEntries(llvm::ArrayRef<RuntimeEntry> Entries,
                                   uint64_t FinalSizeOfImage, Arch TargetArch) {
  uint32_t PreviousEnd = 0;
  bool HavePrevious = false;
  for (const RuntimeEntry &Entry : Entries) {
    if (Entry.Begin >= Entry.End)
      return patchError("runtime-function entry has an empty/reversed range");
    if (Entry.End > FinalSizeOfImage)
      return patchError("runtime-function RVA leaves the patched image");
    if (TargetArch == Arch::X64) {
      if (Entry.Unwind >= FinalSizeOfImage)
        return patchError("unwind-info RVA leaves the patched image");
      if ((Entry.Unwind & 3u) != 0)
        return patchError("x64 unwind-info RVA is not four-byte aligned");
    } else if ((Entry.Unwind & 3u) == 0 && Entry.Unwind >= FinalSizeOfImage) {
      return patchError("ARM xdata RVA leaves the patched image");
    }
    if (HavePrevious && Entry.Begin < PreviousEnd)
      return patchError("runtime-function entries overlap after merging");
    PreviousEnd = Entry.End;
    HavePrevious = true;
  }
  return llvm::Error::success();
}

} // namespace

llvm::Error validateCOFFExceptionSourceIdentityClosure(
    const llvm::Module &Mod, const BinaryImage &Image) {
  auto Functions = validateCanonicalWindowsEHIdentity(Mod, Image);
  if (!Functions)
    return Functions.takeError();
  return llvm::Error::success();
}

llvm::Error
validateCOFFCxxGroupReplacementClosure(const llvm::Module &Mod,
                                       const BinaryImage &Image,
                                       llvm::ArrayRef<va_t> ReplacedEntries) {
  auto Inventory = buildCxxSourceGroupInventory(Image);
  if (!Inventory)
    return Inventory.takeError();
  auto Contracts =
      validateReplacedCxxGroupContracts(Mod, *Inventory, ReplacedEntries);
  if (!Contracts)
    return Contracts.takeError();
  return llvm::Error::success();
}

llvm::Error retainCOFFCxxGroupRewriteContractsForReplacement(
    llvm::Module &Mod, const BinaryImage &Image,
    llvm::ArrayRef<va_t> ReplacedEntries) {
  auto Inventory = buildCxxSourceGroupInventory(Image);
  if (!Inventory)
    return Inventory.takeError();
  auto Contracts =
      validateReplacedCxxGroupContracts(Mod, *Inventory, ReplacedEntries);
  if (!Contracts)
    return Contracts.takeError();
  return exception_rewrite::setCxxGroupRewriteContracts(Mod, *Contracts);
}

std::optional<va_t> findCOFFExceptionPersonalityVA(const BinaryImage &Image,
                                                   llvm::StringRef SymbolName) {
  SymbolName.consume_front("\01");
  const std::optional<va_t> AddressAlias = autoFunctionAddress(SymbolName);
  for (const ExceptionFunction &EH : Image.ExceptionMetadata.Functions) {
    if (EH.Personality == ExceptionPersonality::None ||
        EH.Personality == ExceptionPersonality::Unknown ||
        EH.PersonalityVA == 0)
      continue;
    if (AddressAlias
            ? *AddressAlias != EH.PersonalityVA
            : SymbolName != getExceptionPersonalityName(EH.Personality))
      continue;
    const Segment *Target = Image.getSegmentFor(EH.PersonalityVA);
    if (Target && Target->isExecutable() && Image.readVA(EH.PersonalityVA, 1))
      return EH.PersonalityVA;
  }
  return std::nullopt;
}

llvm::Expected<COFFExceptionPatchPlan>
planCOFFExceptionPatch(const llvm::Module &Mod, const BinaryImage &Image,
                       Arch TargetArch) {
  auto Contracts = exception_rewrite::validateExceptionRewriteContracts(Mod);
  if (!Contracts)
    return Contracts.takeError();

  COFFExceptionPatchPlan Plan;
  if (hasUnsupportedGeneratedGuardMode(Image.DynInfo.GuardFlags))
    return patchError(
        "input uses a guard instrumentation mode not supported by generated "
        "code");
  if (Image.ExceptionMetadata.ParseStatus == ExceptionParseStatus::Malformed)
    return patchError("input exception directory is malformed");

  auto CanonicalFunctions = validateCanonicalWindowsEHIdentity(Mod, Image);
  if (!CanonicalFunctions)
    return CanonicalFunctions.takeError();

  std::vector<va_t> CanonicalEntries;
  CanonicalEntries.reserve(CanonicalFunctions->size());
  for (const CanonicalWindowsEHFunction &Entry : *CanonicalFunctions)
    CanonicalEntries.push_back(Entry.Source->CodeRange.Begin);
  auto CxxGroupInventory = buildCxxSourceGroupInventory(Image);
  if (!CxxGroupInventory)
    return CxxGroupInventory.takeError();
  auto CxxGroupContracts = validateReplacedCxxGroupContracts(
      Mod, *CxxGroupInventory, CanonicalEntries,
      /*RequireExactContractSet=*/true);
  if (!CxxGroupContracts)
    return CxxGroupContracts.takeError();

  for (const CanonicalWindowsEHFunction &Entry : *CanonicalFunctions) {
    const llvm::Function &F = *Entry.Function;
    const ExceptionFunction &EH = *Entry.Source;
    if (llvm::Error Err = validateExceptionFunction(F, EH, TargetArch))
      return std::move(Err);
    Plan.ExceptionFunctionEntries.push_back(EH.CodeRange.Begin);
    if (EH.Personality != ExceptionPersonality::None)
      Plan.LanguageExceptionFunctionEntries.push_back(EH.CodeRange.Begin);
  }
  if ((TargetArch == Arch::ARM || TargetArch == Arch::AArch64) &&
      !Plan.ExceptionFunctionEntries.empty() &&
      std::any_of(Image.ExceptionMetadata.Functions.begin(),
                  Image.ExceptionMetadata.Functions.end(),
                  [](const ExceptionFunction &EH) {
                    return EH.Kind == RuntimeFunctionKind::Fragment;
                  }))
    return patchError(
        "ARM image contains independently addressable function fragments; "
        "their host association is not provable for rewrite");
  return Plan;
}

llvm::Expected<COFFExceptionDirectoryUpdate> prepareCOFFExceptionDirectory(
    llvm::ArrayRef<uint8_t> OriginalBinary, const BinaryImage &Image,
    CompiledImage &Compiled, llvm::ArrayRef<va_t> PatchedOriginalEntries,
    llvm::ArrayRef<std::pair<va_t, va_t>> PatchedEntryMappings,
    uint64_t NewSectionVA, Arch TargetArch) {
  return prepareCOFFExceptionDirectory(
      OriginalBinary, Image, Compiled, PatchedOriginalEntries,
      PatchedEntryMappings, NewSectionVA, TargetArch,
      /*RewriteModule=*/nullptr);
}

llvm::Expected<COFFExceptionDirectoryUpdate> prepareCOFFExceptionDirectory(
    llvm::ArrayRef<uint8_t> OriginalBinary, const BinaryImage &Image,
    CompiledImage &Compiled, llvm::ArrayRef<va_t> PatchedOriginalEntries,
    llvm::ArrayRef<std::pair<va_t, va_t>> PatchedEntryMappings,
    uint64_t NewSectionVA, Arch TargetArch, const llvm::Module *RewriteModule) {
  COFFExceptionDirectoryUpdate Update;
  if (OriginalBinary.empty())
    return patchError("empty input image");
  auto CxxGroupInventory = buildCxxSourceGroupInventory(Image);
  if (!CxxGroupInventory)
    return CxxGroupInventory.takeError();
  auto ReplacedCxxGroups =
      selectReplacedCxxSourceGroups(*CxxGroupInventory, PatchedOriginalEntries);
  if (!ReplacedCxxGroups)
    return ReplacedCxxGroups.takeError();
  std::vector<exception_rewrite::ResolvedCxxGroupRewriteContract>
      ResolvedCxxGroups;
  if (!ReplacedCxxGroups->empty()) {
    if (!RewriteModule)
      return patchError(
          "C++ FuncInfo group installation requires the rewrite module");
    auto Resolved =
        exception_rewrite::validateAndResolveCxxGroupRewriteContracts(
            *RewriteModule, *ReplacedCxxGroups, Compiled);
    if (!Resolved)
      return Resolved.takeError();
    ResolvedCxxGroups = std::move(*Resolved);

    std::set<va_t> MappedSources;
    for (const auto &[SourceVA, GeneratedVA] : PatchedEntryMappings) {
      (void)GeneratedVA;
      if (!MappedSources.insert(SourceVA).second)
        return patchError("patched entry mappings contain duplicate source "
                          "identity 0x" +
                          llvm::utohexstr(SourceVA));
    }
    for (const exception_rewrite::ResolvedCxxGroupRewriteContract &Group :
         ResolvedCxxGroups) {
      for (const exception_rewrite::ResolvedCxxGroupMemberOwner &Member :
           Group.Members) {
        const auto Mapping =
            llvm::find_if(PatchedEntryMappings, [&](const auto &Candidate) {
              return Candidate.first == Member.SourceMemberVA;
            });
        if (Mapping == PatchedEntryMappings.end() ||
            Mapping->second != Member.GeneratedOwnerVA)
          return patchError(
              "C++ FuncInfo group 0x" + llvm::utohexstr(Group.GroupIdentity) +
              " member 0x" + llvm::utohexstr(Member.SourceMemberVA) +
              " lacks its exact generated-owner mapping");
      }
    }
  }

  std::map<va_t, size_t> ResolvedCxxGroupBySourceMember;
  for (size_t I = 0; I < ResolvedCxxGroups.size(); ++I) {
    for (const exception_rewrite::ResolvedCxxGroupMemberOwner &Member :
         ResolvedCxxGroups[I].Members) {
      if (!ResolvedCxxGroupBySourceMember.emplace(Member.SourceMemberVA, I)
               .second)
        return patchError(
            "resolved C++ source member belongs to more than one group");
    }
  }

  auto PE = locatePEHeaders(const_cast<uint8_t *>(OriginalBinary.data()),
                            OriginalBinary.size());
  if (!PE.valid())
    return patchError("invalid PE headers");
  const llvm::object::data_directory *Directory =
      getPEDataDirectory(PE, llvm::COFF::EXCEPTION_TABLE);
  const bool HasOriginal = Directory &&
                           Directory->RelativeVirtualAddress != 0 &&
                           Directory->Size != 0;

  std::vector<const CompiledSection *> GeneratedPDataSections;
  for (const CompiledSection &Section : Compiled.Sections) {
    llvm::StringRef Name(Section.Name);
    if ((Name == ".pdata" || Name.starts_with(".pdata$")) && Section.Size != 0)
      GeneratedPDataSections.push_back(&Section);
  }

  std::vector<ExceptionAddressRange> SupersededRanges;
  std::set<uint32_t> SupersededRecords = collectSupersededRuntimeRecords(
      Image, PatchedOriginalEntries, SupersededRanges);
  const bool NeedsUpdate = !GeneratedPDataSections.empty() ||
                           !SupersededRecords.empty() ||
                           !SupersededRanges.empty();
  if (!NeedsUpdate)
    return Update;
  if (TargetArch != Arch::X64 && TargetArch != Arch::AArch64 &&
      TargetArch != Arch::ARM)
    return patchError("native exception-directory installation is unsupported "
                      "for this architecture");
  if (NewSectionVA < Image.Base)
    return patchError("new section precedes the PE image base");
  uint64_t NewSectionRVA = NewSectionVA - Image.Base;
  if (NewSectionRVA > std::numeric_limits<uint32_t>::max())
    return patchError("new section exceeds PE RVA limits");

  for (va_t PatchedEntry : PatchedOriginalEntries) {
    const ExceptionFunction *PatchedEH = nullptr;
    for (const ExceptionFunction &EH : Image.ExceptionMetadata.Functions) {
      if (EH.Kind == RuntimeFunctionKind::Primary &&
          EH.CodeRange.Begin == PatchedEntry) {
        PatchedEH = &EH;
        break;
      }
    }
    if (!PatchedEH) {
      if (Image.ExceptionMetadata.findFunction(PatchedEntry))
        return patchError("patched entry does not identify a primary runtime "
                          "function");
      continue;
    }
    if (llvm::Error Err = validateExceptionRecord(
            *PatchedEH, TargetArch,
            llvm::Twine("patched entry 0x") + llvm::utohexstr(PatchedEntry)))
      return std::move(Err);
  }
  const uint64_t RuntimeFunctionSize =
      TargetArch == Arch::X64 ? X64RuntimeFunctionSize : ARMRuntimeFunctionSize;

  auto ReadAtRVA = [&](uint32_t RVA, size_t Size) -> const uint8_t * {
    if (RVA >= NewSectionRVA) {
      uint64_t Offset = uint64_t(RVA) - NewSectionRVA;
      if (rangeInBounds(Offset, Size, Compiled.Bytes.size()))
        return Compiled.Bytes.data() + Offset;
    }
    auto FileOffset = rvaToFileOffset(PE, OriginalBinary, RVA, Size);
    return FileOffset ? OriginalBinary.data() + *FileOffset : nullptr;
  };

  std::vector<RuntimeEntry> Entries;
  if (HasOriginal) {
    if (Directory->Size % RuntimeFunctionSize != 0)
      return patchError("input exception directory is misaligned");
    auto TableOffset = rvaToFileOffset(
        PE, OriginalBinary, Directory->RelativeVirtualAddress, Directory->Size);
    if (!TableOffset)
      return patchError("input exception directory is not file-backed");
    size_t Count = Directory->Size / RuntimeFunctionSize;
    Entries.reserve(Count);
    for (size_t I = 0; I < Count; ++I) {
      const uint8_t *Record =
          OriginalBinary.data() + *TableOffset + I * RuntimeFunctionSize;
      bool IsZero = true;
      for (uint64_t W = 0; W < RuntimeFunctionSize / 4; ++W)
        IsZero &= readLE<uint32_t>(Record + W * 4) == 0;
      if (IsZero)
        continue;
      auto EntryOrErr = decodeRuntimeEntry(Record, TargetArch, ReadAtRVA);
      if (!EntryOrErr)
        return EntryOrErr.takeError();
      RuntimeEntry Entry = *EntryOrErr;
      uint64_t RecordRVA =
          uint64_t(Directory->RelativeVirtualAddress) + I * RuntimeFunctionSize;
      if (RecordRVA <= std::numeric_limits<uint32_t>::max() &&
          SupersededRecords.count(static_cast<uint32_t>(RecordRVA)))
        continue;
      if (overlapsAny(Entry, Image.Base, SupersededRanges))
        continue;
      Entries.push_back(Entry);
    }
  }

  std::vector<RuntimeEntry> GeneratedEntries;
  for (const CompiledSection *GeneratedPData : GeneratedPDataSections) {
    if (!GeneratedPData->IsAllocated ||
        GeneratedPData->Offset > InvalidVA - NewSectionVA ||
        GeneratedPData->VA != NewSectionVA + GeneratedPData->Offset ||
        GeneratedPData->Alignment < alignof(uint32_t) ||
        (GeneratedPData->Alignment & (GeneratedPData->Alignment - 1)) != 0)
      return patchError("generated .pdata has invalid placement traits");
    if (GeneratedPData->Size % RuntimeFunctionSize != 0 ||
        !rangeInBounds(GeneratedPData->Offset, GeneratedPData->Size,
                       Compiled.Bytes.size()))
      return patchError("generated .pdata has an invalid extent");
    const uint8_t *Bytes = Compiled.Bytes.data() + GeneratedPData->Offset;
    size_t Count = GeneratedPData->Size / RuntimeFunctionSize;
    Entries.reserve(Entries.size() + Count);
    for (size_t I = 0; I < Count; ++I) {
      const uint8_t *Record = Bytes + I * RuntimeFunctionSize;
      bool IsZero = true;
      for (uint64_t W = 0; W < RuntimeFunctionSize / 4; ++W)
        IsZero &= readLE<uint32_t>(Record + W * 4) == 0;
      if (IsZero)
        continue;
      auto EntryOrErr = decodeRuntimeEntry(Record, TargetArch, ReadAtRVA);
      if (!EntryOrErr)
        return EntryOrErr.takeError();
      GeneratedEntries.push_back(*EntryOrErr);
      Entries.push_back(*EntryOrErr);
    }
  }

  struct PlannedGeneratedLanguageOwner {
    bool Assigned = false;
    COFFGeneratedLanguageModel Model = COFFGeneratedLanguageModel::SEH;
    COFFGeneratedLanguageOwnerRole Role =
        COFFGeneratedLanguageOwnerRole::MappedRoot;
    uint32_t LanguageGroupRVA = 0;
    bool GSWrapped = false;
    uint32_t GSCookieHeader = 0;
  };
  std::vector<PlannedGeneratedLanguageOwner> GeneratedLanguagePlans(
      GeneratedEntries.size());
  auto AssignGeneratedLanguageOwner =
      [&](size_t Index, COFFGeneratedLanguageModel Model,
          COFFGeneratedLanguageOwnerRole Role,
          uint32_t LanguageGroupRVA) -> llvm::Error {
    if (Index >= GeneratedEntries.size() ||
        !GeneratedEntries[Index].HasLanguageHandler)
      return patchError("generated language receipt names a non-language "
                        "runtime record");
    PlannedGeneratedLanguageOwner &Plan = GeneratedLanguagePlans[Index];
    if (Plan.Assigned)
      return patchError(
          "generated language runtime record has ambiguous source ownership");
    Plan.Assigned = true;
    Plan.Model = Model;
    Plan.Role = Role;
    Plan.LanguageGroupRVA = LanguageGroupRVA;
    return llvm::Error::success();
  };

  for (size_t I = 0; I < GeneratedEntries.size(); ++I) {
    if (!GeneratedEntries[I].HasLanguageHandler)
      continue;
    for (size_t J = I + 1; J < GeneratedEntries.size(); ++J)
      if (GeneratedEntries[J].HasLanguageHandler &&
          GeneratedEntries[I] == GeneratedEntries[J])
        return patchError(
            "generated language runtime records contain a duplicate owner");
  }

  const bool HasGeneratedLanguageOwners = std::any_of(
      GeneratedEntries.begin(), GeneratedEntries.end(),
      [](const RuntimeEntry &Entry) { return Entry.HasLanguageHandler; });
  if (HasGeneratedLanguageOwners &&
      (!Compiled.FunctionRangesValid ||
       !llvm::mc_rewrite::validateRewriteFunctionRanges(
           Compiled.FunctionRanges, Compiled.FunctionOwnerAddrs)))
    return patchError(
        "generated language owners lack valid compiler function ranges");
  if (HasGeneratedLanguageOwners &&
      (!Compiled.WinEHSemanticsValid ||
       !llvm::mc_rewrite::validateRewriteWinEHSemanticRecords(
           Compiled.WinEHSemanticRecords, Compiled.SourceFunctionOwners,
           Compiled.FunctionRanges, Compiled.FunctionOwnerAddrs)))
    return patchError(
        "generated language owners lack valid compiler EH semantics");
  auto ValidateGeneratedRuntimeExtent =
      [&](const RuntimeEntry &Entry, va_t OwnerVA,
          llvm::StringRef ExpectedOwnerSymbol = {})
      -> llvm::Expected<const llvm::mc_rewrite::RewriteFunctionRange *> {
    const llvm::mc_rewrite::RewriteFunctionRange *ExactRange = nullptr;
    for (const llvm::mc_rewrite::RewriteFunctionRange &Range :
         Compiled.FunctionRanges) {
      if (Range.BeginVA != OwnerVA ||
          (!ExpectedOwnerSymbol.empty() &&
           Range.OwnerSymbol != ExpectedOwnerSymbol))
        continue;
      if (ExactRange)
        return patchError(
            "generated language owner has ambiguous compiler ranges");
      ExactRange = &Range;
    }
    if (!ExactRange)
      return patchError("generated language owner has no exact compiler range");
    if (ExactRange->OwnerVA != OwnerVA)
      return patchError(
          "generated language runtime record does not begin at its compiler "
          "owner");
    if (OwnerVA < Image.Base || ExactRange->EndVA < Image.Base ||
        OwnerVA - Image.Base > std::numeric_limits<uint32_t>::max() ||
        ExactRange->EndVA - Image.Base > std::numeric_limits<uint32_t>::max())
      return patchError("generated language compiler range exceeds PE limits");
    if (Entry.Begin != OwnerVA - Image.Base ||
        Entry.End != ExactRange->EndVA - Image.Base)
      return patchError(
          "generated language runtime extent does not match its compiler "
          "owner");
    return ExactRange;
  };

  for (const auto &[OriginalVA, GeneratedVA] : PatchedEntryMappings) {
    const auto ResolvedGroupIt =
        ResolvedCxxGroupBySourceMember.find(OriginalVA);
    if (ResolvedGroupIt != ResolvedCxxGroupBySourceMember.end() &&
        OriginalVA !=
            ResolvedCxxGroups[ResolvedGroupIt->second].CanonicalSourceOwnerVA)
      continue;
    auto EHIt =
        std::find_if(Image.ExceptionMetadata.Functions.begin(),
                     Image.ExceptionMetadata.Functions.end(),
                     [&](const ExceptionFunction &EH) {
                       return EH.Kind == RuntimeFunctionKind::Primary &&
                              EH.CodeRange.Begin == OriginalVA &&
                              EH.Personality != ExceptionPersonality::None;
                     });
    if (EHIt == Image.ExceptionMetadata.Functions.end())
      continue;
    if (GeneratedVA < Image.Base ||
        GeneratedVA - Image.Base > std::numeric_limits<uint32_t>::max())
      return patchError("generated language-EH entry exceeds PE RVA limits");
    uint32_t GeneratedRVA = static_cast<uint32_t>(GeneratedVA - Image.Base);
    std::optional<size_t> GeneratedOwnerIndex;
    for (size_t I = 0; I < GeneratedEntries.size(); ++I) {
      const RuntimeEntry &Entry = GeneratedEntries[I];
      const bool HasExactOwner = Entry.Begin == GeneratedRVA;
      if (!HasExactOwner || !Entry.HasLanguageHandler ||
          Entry.Handler > InvalidVA - Image.Base)
        continue;
      const va_t HandlerVA = Image.Base + Entry.Handler;
      if (HandlerVA !=
          normalizeCodeAddress(EHIt->PersonalityVA, TargetArch, Image.Mode))
        continue;
      if (GeneratedOwnerIndex)
        return patchError(
            "generated language-EH function has ambiguous runtime owners");
      GeneratedOwnerIndex = I;
    }
    if (!GeneratedOwnerIndex)
      return patchError(
          "generated language-EH function has no matching language-handler "
          "runtime record");
    const RuntimeEntry &GeneratedOwner = GeneratedEntries[*GeneratedOwnerIndex];
    llvm::Expected<const llvm::mc_rewrite::RewriteFunctionRange *>
        GeneratedOwnerRange =
            ValidateGeneratedRuntimeExtent(GeneratedOwner, GeneratedVA);
    if (!GeneratedOwnerRange)
      return GeneratedOwnerRange.takeError();
    if (!(*GeneratedOwnerRange)->ParentOwnerSymbol.empty() ||
        (*GeneratedOwnerRange)->ParentOwnerVA != 0)
      return patchError(
          "generated language mapped root is not a direct compiler owner");

    if (EHIt->Personality == ExceptionPersonality::CxxFrameHandler3) {
      if (!isFH3OutputArchitecture(TargetArch) ||
          GeneratedOwner.HandlerData == 0)
        return patchError("generated C++ EH owner has invalid handler data");
      const uint8_t *GroupBytes =
          ReadAtRVA(GeneratedOwner.HandlerData, sizeof(uint32_t));
      if (!GroupBytes)
        return patchError("generated C++ EH handler data is truncated");
      const uint32_t LanguageGroupRVA = readLE<uint32_t>(GroupBytes);
      if (LanguageGroupRVA == 0)
        return patchError("generated C++ EH owner has a zero FuncInfo RVA");

      if (ResolvedGroupIt == ResolvedCxxGroupBySourceMember.end()) {
        if (llvm::Error Err = AssignGeneratedLanguageOwner(
                *GeneratedOwnerIndex, COFFGeneratedLanguageModel::CxxFH3,
                COFFGeneratedLanguageOwnerRole::MappedRoot, LanguageGroupRVA))
          return std::move(Err);

        std::set<size_t> AuthenticatedRuntimeOwners = {*GeneratedOwnerIndex};
        for (size_t I = 0; I < GeneratedEntries.size(); ++I) {
          if (I == *GeneratedOwnerIndex)
            continue;
          const RuntimeEntry &Candidate = GeneratedEntries[I];
          if (!Candidate.HasLanguageHandler ||
              Candidate.Handler != GeneratedOwner.Handler ||
              Candidate.HandlerData == 0)
            continue;
          const uint8_t *CandidateGroupBytes =
              ReadAtRVA(Candidate.HandlerData, sizeof(uint32_t));
          if (!CandidateGroupBytes)
            return patchError("generated C++ EH handler data is truncated");
          if (readLE<uint32_t>(CandidateGroupBytes) != LanguageGroupRVA)
            continue;
          if (Candidate.Begin > InvalidVA - Image.Base)
            return patchError(
                "generated C++ auxiliary owner exceeds PE address limits");
          const va_t CandidateOwnerVA = Image.Base + Candidate.Begin;
          llvm::Expected<const llvm::mc_rewrite::RewriteFunctionRange *>
              CandidateRange =
                  ValidateGeneratedRuntimeExtent(Candidate, CandidateOwnerVA);
          if (!CandidateRange)
            return CandidateRange.takeError();
          if ((*CandidateRange)->ParentOwnerSymbol !=
                  (*GeneratedOwnerRange)->OwnerSymbol ||
              (*CandidateRange)->ParentOwnerVA !=
                  (*GeneratedOwnerRange)->OwnerVA)
            return patchError(
                "generated C++ auxiliary owner is not compiler-derived from "
                "its mapped root");
          if (!AuthenticatedRuntimeOwners.insert(I).second)
            return patchError(
                "generated C++ auxiliary runtime owner is duplicated");
          if (llvm::Error Err = AssignGeneratedLanguageOwner(
                  I, COFFGeneratedLanguageModel::CxxFH3,
                  COFFGeneratedLanguageOwnerRole::CxxAuxiliary,
                  LanguageGroupRVA))
            return std::move(Err);
        }
      } else {
        const exception_rewrite::ResolvedCxxGroupRewriteContract
            &ResolvedGroup = ResolvedCxxGroups[ResolvedGroupIt->second];
        std::set<uint32_t> ExpectedOwnerRVAs;
        std::set<size_t> ResolvedRuntimeOwners;
        for (const exception_rewrite::ResolvedCxxGroupMemberOwner &Member :
             ResolvedGroup.Members) {
          if (Member.GeneratedOwnerVA < Image.Base ||
              Member.GeneratedOwnerVA - Image.Base >
                  std::numeric_limits<uint32_t>::max())
            return patchError(
                "resolved C++ generated owner exceeds PE RVA limits");
          const uint32_t ExpectedOwnerRVA =
              static_cast<uint32_t>(Member.GeneratedOwnerVA - Image.Base);
          if (!ExpectedOwnerRVAs.insert(ExpectedOwnerRVA).second)
            return patchError(
                "resolved C++ group members alias one generated owner");

          std::optional<size_t> ExactRuntimeOwner;
          for (size_t I = 0; I < GeneratedEntries.size(); ++I) {
            const RuntimeEntry &Candidate = GeneratedEntries[I];
            if (Candidate.Begin != ExpectedOwnerRVA ||
                !Candidate.HasLanguageHandler ||
                Candidate.Handler != GeneratedOwner.Handler ||
                Candidate.HandlerData == 0)
              continue;
            const uint8_t *CandidateGroupBytes =
                ReadAtRVA(Candidate.HandlerData, sizeof(uint32_t));
            if (!CandidateGroupBytes)
              return patchError("generated C++ EH handler data is truncated");
            if (readLE<uint32_t>(CandidateGroupBytes) != LanguageGroupRVA)
              continue;
            if (ExactRuntimeOwner)
              return patchError(
                  "resolved C++ member has ambiguous runtime owners");
            ExactRuntimeOwner = I;
          }
          if (!ExactRuntimeOwner)
            return patchError(
                "resolved C++ member has no exact language runtime owner");
          if (!ResolvedRuntimeOwners.insert(*ExactRuntimeOwner).second)
            return patchError(
                "resolved C++ group members share one runtime owner");

          const bool IsCanonical =
              Member.SourceMemberVA == ResolvedGroup.CanonicalSourceOwnerVA;
          if (IsCanonical != (*ExactRuntimeOwner == *GeneratedOwnerIndex))
            return patchError(
                "resolved C++ canonical owner does not match the mapped root");
          llvm::Expected<const llvm::mc_rewrite::RewriteFunctionRange *>
              ExactRuntimeRange = ValidateGeneratedRuntimeExtent(
                  GeneratedEntries[*ExactRuntimeOwner], Member.GeneratedOwnerVA,
                  Member.GeneratedOwnerSymbol);
          if (!ExactRuntimeRange)
            return ExactRuntimeRange.takeError();
          if (IsCanonical) {
            if (*ExactRuntimeRange != *GeneratedOwnerRange)
              return patchError(
                  "resolved C++ canonical owner has a mismatched compiler "
                  "range");
          } else if ((*ExactRuntimeRange)->ParentOwnerSymbol !=
                         (*GeneratedOwnerRange)->OwnerSymbol ||
                     (*ExactRuntimeRange)->ParentOwnerVA !=
                         (*GeneratedOwnerRange)->OwnerVA) {
            return patchError(
                "resolved C++ auxiliary owner is not compiler-derived from "
                "its mapped root");
          }
          if (llvm::Error Err = AssignGeneratedLanguageOwner(
                  *ExactRuntimeOwner, COFFGeneratedLanguageModel::CxxFH3,
                  IsCanonical ? COFFGeneratedLanguageOwnerRole::MappedRoot
                              : COFFGeneratedLanguageOwnerRole::CxxAuxiliary,
                  LanguageGroupRVA))
            return std::move(Err);
        }

        for (size_t I = 0; I < GeneratedEntries.size(); ++I) {
          const RuntimeEntry &Candidate = GeneratedEntries[I];
          if (!Candidate.HasLanguageHandler ||
              Candidate.Handler != GeneratedOwner.Handler ||
              Candidate.HandlerData == 0)
            continue;
          const uint8_t *CandidateGroupBytes =
              ReadAtRVA(Candidate.HandlerData, sizeof(uint32_t));
          if (!CandidateGroupBytes)
            return patchError("generated C++ EH handler data is truncated");
          if (readLE<uint32_t>(CandidateGroupBytes) == LanguageGroupRVA &&
              !ResolvedRuntimeOwners.count(I))
            return patchError(
                "generated C++ EH group has a runtime owner outside its "
                "resolved member set");
        }
      }
    } else if (isCxxFH4SourcePersonality(EHIt->Personality)) {
      const bool IsGSWrapped =
          EHIt->Personality == ExceptionPersonality::GSHandlerCheckEH4;
      if (TargetArch != Arch::X64 || GeneratedOwner.HandlerData == 0 ||
          ResolvedGroupIt != ResolvedCxxGroupBySourceMember.end())
        return patchError("generated C++ EH4 owner has invalid handler data");
      const size_t HandlerDataSize =
          IsGSWrapped ? 2 * sizeof(uint32_t) : sizeof(uint32_t);
      const uint8_t *GroupBytes =
          ReadAtRVA(GeneratedOwner.HandlerData, HandlerDataSize);
      if (!GroupBytes)
        return patchError("generated C++ EH4 handler data is truncated");
      const uint32_t LanguageGroupRVA = readLE<uint32_t>(GroupBytes);
      if (LanguageGroupRVA == 0)
        return patchError("generated C++ EH4 owner has a zero FuncInfo RVA");
      auto Layout = parseGeneratedFH4Layout(LanguageGroupRVA, ReadAtRVA);
      if (!Layout)
        return Layout.takeError();
      if (llvm::Error Err = validateBoundedGeneratedFH4Layout(*Layout))
        return std::move(Err);
      if (llvm::Error Err = AssignGeneratedLanguageOwner(
              *GeneratedOwnerIndex, COFFGeneratedLanguageModel::CxxFH4,
              COFFGeneratedLanguageOwnerRole::MappedRoot, LanguageGroupRVA))
        return std::move(Err);
      if (IsGSWrapped) {
        const uint32_t GSCookieHeader =
            readLE<uint32_t>(GroupBytes + sizeof(uint32_t));
        if (!isValidBoundedX64GSCookieHeader(GSCookieHeader))
          return patchError(
              "generated C++ EH4 owner has an invalid GS cookie header");
        GeneratedLanguagePlans[*GeneratedOwnerIndex].GSWrapped = true;
        GeneratedLanguagePlans[*GeneratedOwnerIndex].GSCookieHeader =
            GSCookieHeader;
      }
    } else if (EHIt->Personality == ExceptionPersonality::CSpecificHandler) {
      if (llvm::Error Err = AssignGeneratedLanguageOwner(
              *GeneratedOwnerIndex, COFFGeneratedLanguageModel::SEH,
              COFFGeneratedLanguageOwnerRole::MappedRoot,
              /*LanguageGroupRVA=*/0))
        return std::move(Err);
    } else {
      return patchError(
          "generated language owner uses an unsupported personality model");
    }

    if ((TargetArch == Arch::ARM || TargetArch == Arch::AArch64) &&
        EHIt->Personality == ExceptionPersonality::CSpecificHandler) {
      const llvm::StringRef Architecture =
          TargetArch == Arch::ARM ? "ARM32" : "ARM64";
      std::optional<uint32_t> XDataEndRVA;
      const va_t HandlerDataVA = Image.Base + GeneratedOwner.HandlerData;
      for (const CompiledSection &Section : Compiled.Sections) {
        const llvm::StringRef Name(Section.Name);
        if ((Name != ".xdata" && !Name.starts_with(".xdata$")) ||
            Section.Size == 0 || HandlerDataVA < Section.VA ||
            HandlerDataVA - Section.VA >= Section.Size)
          continue;
        if (XDataEndRVA)
          return patchError("generated " + Architecture +
                            " SEH handler data has ambiguous xdata owners");
        if (!Section.IsAllocated || !Section.IsInImage ||
            Section.Offset > InvalidVA - NewSectionVA ||
            Section.VA != NewSectionVA + Section.Offset ||
            Section.Alignment < alignof(uint32_t) ||
            (Section.Alignment & (Section.Alignment - 1)) != 0 ||
            (Section.Offset & (alignof(uint32_t) - 1)) != 0 ||
            (Section.VA & (alignof(uint32_t) - 1)) != 0)
          return patchError("generated " + Architecture +
                            " SEH xdata has invalid placement traits");
        if (Section.VA < Image.Base || Section.Size > InvalidVA - Section.VA ||
            Section.VA + Section.Size - Image.Base >
                std::numeric_limits<uint32_t>::max() ||
            !rangeInBounds(Section.Offset, Section.Size, Compiled.Bytes.size()))
          return patchError("generated " + Architecture +
                            " SEH xdata section has an invalid extent");
        XDataEndRVA =
            static_cast<uint32_t>(Section.VA + Section.Size - Image.Base);
      }
      if (!XDataEndRVA)
        return patchError("generated " + Architecture +
                          " SEH handler data has no xdata owner");
      if (llvm::Error Err = validateGeneratedARMCatchAllTable(
              GeneratedOwner, TargetArch, *XDataEndRVA, ReadAtRVA))
        return std::move(Err);
    }
  }

  for (size_t I = 0; I < GeneratedEntries.size(); ++I) {
    if (GeneratedEntries[I].HasLanguageHandler &&
        !GeneratedLanguagePlans[I].Assigned)
      return patchError(
          "generated language runtime record has no mapped source owner");
  }

  auto FindGeneratedSection = [&](uint32_t RVA, size_t Size, bool RequireXData,
                                  const llvm::Twine &Description)
      -> llvm::Expected<const CompiledSection *> {
    if (Size == 0 || RVA > InvalidVA - Image.Base ||
        uint64_t(RVA) + Size > std::numeric_limits<uint32_t>::max())
      return patchError("generated " + Description + " has an invalid RVA");
    const va_t Address = Image.Base + RVA;
    const CompiledSection *Owner = nullptr;
    for (const CompiledSection &Section : Compiled.Sections) {
      const llvm::StringRef Name(Section.Name);
      if (RequireXData && Name != ".xdata" && !Name.starts_with(".xdata$"))
        continue;
      if (Section.Size == 0 || Address < Section.VA ||
          Address - Section.VA > Section.Size ||
          Size > Section.Size - (Address - Section.VA))
        continue;
      if (Owner)
        return patchError("generated " + Description +
                          " has ambiguous section ownership");
      if (!Section.IsAllocated || !Section.IsInImage ||
          Section.Offset > InvalidVA - NewSectionVA ||
          Section.VA != NewSectionVA + Section.Offset ||
          Section.Alignment < alignof(uint32_t) ||
          (Section.Alignment & (Section.Alignment - 1)) != 0 ||
          (Section.Offset & (alignof(uint32_t) - 1)) != 0 ||
          (Section.VA & (alignof(uint32_t) - 1)) != 0)
        return patchError("generated " + Description +
                          " has invalid placement traits");
      if (Section.VA < Image.Base || Section.Size > InvalidVA - Section.VA ||
          Section.VA + Section.Size - Image.Base >
              std::numeric_limits<uint32_t>::max() ||
          !rangeInBounds(Section.Offset, Section.Size, Compiled.Bytes.size()))
        return patchError("generated " + Description +
                          " section has an invalid extent");
      Owner = &Section;
    }
    if (!Owner)
      return patchError("generated " + Description +
                        " has no generated section owner");
    return Owner;
  };

  for (size_t I = 0; I < GeneratedEntries.size(); ++I) {
    if (!GeneratedLanguagePlans[I].Assigned)
      continue;
    const RuntimeEntry &Entry = GeneratedEntries[I];
    auto UnwindOwner = FindGeneratedSection(Entry.Unwind, sizeof(uint32_t),
                                            /*RequireXData=*/true,
                                            "language unwind information");
    if (!UnwindOwner)
      return UnwindOwner.takeError();
    const size_t HandlerDataSize = GeneratedLanguagePlans[I].GSWrapped
                                       ? 2 * sizeof(uint32_t)
                                       : sizeof(uint32_t);
    auto HandlerDataOwner = FindGeneratedSection(
        Entry.HandlerData, HandlerDataSize,
        /*RequireXData=*/true, "language handler data");
    if (!HandlerDataOwner)
      return HandlerDataOwner.takeError();
    if ((*UnwindOwner)->Kind !=
            llvm::mc_rewrite::RewriteSectionKind::ReadOnlyData ||
        (*HandlerDataOwner)->Kind !=
            llvm::mc_rewrite::RewriteSectionKind::ReadOnlyData)
      return patchError(
          "generated language metadata has invalid section permissions");
    if (GeneratedLanguagePlans[I].LanguageGroupRVA != 0) {
      const size_t GroupHeaderSize =
          GeneratedLanguagePlans[I].Model == COFFGeneratedLanguageModel::CxxFH3
              ? FH3FuncInfoSize
              : 1;
      auto GroupOwner = FindGeneratedSection(
          GeneratedLanguagePlans[I].LanguageGroupRVA, GroupHeaderSize,
          /*RequireXData=*/true, "C++ FuncInfo");
      if (!GroupOwner)
        return GroupOwner.takeError();
      if ((*GroupOwner)->Kind !=
          llvm::mc_rewrite::RewriteSectionKind::ReadOnlyData)
        return patchError(
            "generated C++ FuncInfo has invalid section permissions");
    }
  }

  using SemanticKind = llvm::mc_rewrite::RewriteWinEHSemanticKind;
  using SemanticToken = llvm::mc_rewrite::RewriteWinEHSemanticToken;
  using SemanticTokenIdentity = std::array<uint64_t, 7>;
  auto TokenIdentity = [](const SemanticToken &Token) {
    return SemanticTokenIdentity{static_cast<uint64_t>(Token.Kind),
                                 Token.Region,
                                 Token.Clause,
                                 Token.Digest[0],
                                 Token.Digest[1],
                                 Token.Digest[2],
                                 Token.Digest[3]};
  };
  auto ToRVA = [&](uint64_t VA) -> std::optional<uint32_t> {
    if (VA < Image.Base ||
        VA - Image.Base > std::numeric_limits<uint32_t>::max())
      return std::nullopt;
    return static_cast<uint32_t>(VA - Image.Base);
  };
  auto FindSourceEH = [&](va_t SourceVA) -> const ExceptionFunction * {
    const ExceptionFunction *Result = nullptr;
    for (const ExceptionFunction &EH : Image.ExceptionMetadata.Functions) {
      if (EH.Kind != RuntimeFunctionKind::Primary ||
          EH.CodeRange.Begin != SourceVA)
        continue;
      if (Result)
        return nullptr;
      Result = &EH;
    }
    return Result;
  };

  std::map<va_t, va_t> GeneratedOwnerBySource;
  std::map<va_t, va_t> SourceByGeneratedOwner;
  for (const auto &[SourceVA, GeneratedVA] : PatchedEntryMappings)
    if (!GeneratedOwnerBySource.emplace(SourceVA, GeneratedVA).second ||
        !SourceByGeneratedOwner.emplace(GeneratedVA, SourceVA).second)
      return patchError(
          "patched entry mappings are not a one-to-one semantic mapping");

  std::map<SemanticTokenIdentity, size_t> RequiredSemanticTokens;
  for (size_t I = 0; I < GeneratedEntries.size(); ++I) {
    const PlannedGeneratedLanguageOwner &Plan = GeneratedLanguagePlans[I];
    if (!Plan.Assigned ||
        Plan.Role != COFFGeneratedLanguageOwnerRole::MappedRoot)
      continue;
    if (GeneratedEntries[I].Begin > InvalidVA - Image.Base)
      return patchError("generated language owner VA overflows");
    const va_t GeneratedOwnerVA = Image.Base + GeneratedEntries[I].Begin;
    const auto SourceIt = SourceByGeneratedOwner.find(GeneratedOwnerVA);
    if (SourceIt == SourceByGeneratedOwner.end())
      return patchError(
          "generated language root has no exact mapped source identity");
    const ExceptionFunction *SourceEH = FindSourceEH(SourceIt->second);
    if (!SourceEH)
      return patchError(
          "generated language root has no unique source exception record");

    if (Plan.Model == COFFGeneratedLanguageModel::SEH) {
      if (!SourceEH->SEH || SourceEH->Cxx)
        return patchError("generated SEH root has a non-SEH source model");
      for (size_t ScopeIndex = 0; ScopeIndex < SourceEH->SEH->Scopes.size();
           ++ScopeIndex) {
        if (ScopeIndex > std::numeric_limits<uint32_t>::max())
          return patchError("source SEH scope index exceeds token limits");
        const auto Token = windows_eh_semantics::getSEHScopeSemanticToken(
            *SourceEH, TargetArch, static_cast<uint32_t>(ScopeIndex));
        if (!Token ||
            !RequiredSemanticTokens.emplace(TokenIdentity(*Token), 1).second)
          return patchError(
              "source SEH scopes do not have unique semantic tokens");
      }
      continue;
    }

    if (!SourceEH->Cxx || SourceEH->SEH)
      return patchError("generated C++ root has a non-C++ source model");
    for (size_t TryIndex = 0; TryIndex < SourceEH->Cxx->TryBlocks.size();
         ++TryIndex) {
      if (TryIndex > std::numeric_limits<uint32_t>::max())
        return patchError("source C++ try index exceeds token limits");
      const CxxTryBlock &Try = SourceEH->Cxx->TryBlocks[TryIndex];
      for (size_t CatchIndex = 0; CatchIndex < Try.Handlers.size();
           ++CatchIndex) {
        if (CatchIndex > std::numeric_limits<uint32_t>::max())
          return patchError("source C++ catch index exceeds token limits");
        const auto Token = windows_eh_semantics::getCxxCatchSemanticToken(
            *SourceEH, TargetArch, static_cast<uint32_t>(TryIndex),
            static_cast<uint32_t>(CatchIndex));
        if (!Token ||
            !RequiredSemanticTokens.emplace(TokenIdentity(*Token), 1).second)
          return patchError(
              "source C++ catches do not have unique semantic tokens");
      }
    }
  }

  using SemanticRowIdentity =
      std::tuple<COFFGeneratedEHSemanticKind, uint32_t, uint32_t, uint32_t>;
  std::set<SemanticRowIdentity> PhysicalSemanticRows;
  auto AddPhysicalRows = [&](COFFGeneratedEHSemanticKind Kind,
                             uint32_t RootOwnerRVA, uint32_t ContainerRVA,
                             uint32_t FirstRVA, uint32_t Count,
                             uint32_t Stride) -> llvm::Error {
    if (Stride == 0 || Count > MaxGeneratedLanguageRecords ||
        uint64_t(FirstRVA) + uint64_t(Count) * Stride >
            std::numeric_limits<uint32_t>::max())
      return patchError("generated EH semantic table has an invalid extent");
    for (uint32_t I = 0; I < Count; ++I) {
      const uint32_t RecordRVA =
          FirstRVA + static_cast<uint32_t>(uint64_t(I) * Stride);
      if (!ReadAtRVA(RecordRVA, Stride) ||
          !PhysicalSemanticRows
               .emplace(Kind, RootOwnerRVA, ContainerRVA, RecordRVA)
               .second)
        return patchError(
            "generated EH semantic table has duplicate or invalid rows");
    }
    return llvm::Error::success();
  };
  for (size_t I = 0; I < GeneratedEntries.size(); ++I) {
    const PlannedGeneratedLanguageOwner &Plan = GeneratedLanguagePlans[I];
    if (!Plan.Assigned ||
        Plan.Role != COFFGeneratedLanguageOwnerRole::MappedRoot)
      continue;
    const RuntimeEntry &Entry = GeneratedEntries[I];
    if (Plan.Model == COFFGeneratedLanguageModel::SEH) {
      const uint8_t *Header = ReadAtRVA(Entry.HandlerData, sizeof(uint32_t));
      if (!Header || Entry.HandlerData > std::numeric_limits<uint32_t>::max() -
                                             sizeof(uint32_t))
        return patchError("generated SEH scope table is truncated");
      const uint32_t ScopeCount = readLE<uint32_t>(Header);
      if (ScopeCount > MaxGeneratedLanguageRecords)
        return patchError("generated SEH scope table has too many rows");
      const size_t ScopeTableSize =
          sizeof(uint32_t) + size_t(ScopeCount) * 4 * sizeof(uint32_t);
      auto ScopeTableOwner =
          FindGeneratedSection(Entry.HandlerData, ScopeTableSize,
                               /*RequireXData=*/true, "SEH scope table");
      if (!ScopeTableOwner)
        return ScopeTableOwner.takeError();
      if ((*ScopeTableOwner)->Kind !=
          llvm::mc_rewrite::RewriteSectionKind::ReadOnlyData)
        return patchError(
            "generated SEH scope table has invalid section permissions");
      if (llvm::Error Err =
              AddPhysicalRows(COFFGeneratedEHSemanticKind::SEHScope,
                              Entry.Begin, Entry.HandlerData + sizeof(uint32_t),
                              Entry.HandlerData + sizeof(uint32_t), ScopeCount,
                              4 * sizeof(uint32_t)))
        return std::move(Err);
      continue;
    }

    if (Plan.Model == COFFGeneratedLanguageModel::CxxFH4) {
      auto Layout = parseGeneratedFH4Layout(Plan.LanguageGroupRVA, ReadAtRVA);
      if (!Layout)
        return Layout.takeError();
      if (llvm::Error Err = validateBoundedGeneratedFH4Layout(*Layout))
        return std::move(Err);
      auto ValidateRange = [&](const coff_fh4::ByteRange &Range,
                               const llvm::Twine &Description) -> llvm::Error {
        auto Owner = FindGeneratedSection(Range.BeginRVA, Range.size(),
                                          /*RequireXData=*/true, Description);
        if (!Owner)
          return Owner.takeError();
        if ((*Owner)->Kind !=
            llvm::mc_rewrite::RewriteSectionKind::ReadOnlyData)
          return patchError("generated " + Description +
                            " has invalid section permissions");
        return llvm::Error::success();
      };
      if (llvm::Error Err =
              ValidateRange(Layout->HeaderRange, "C++ EH4 FuncInfo"))
        return std::move(Err);
      if (llvm::Error Err =
              ValidateRange(Layout->Unwind->Range, "C++ EH4 unwind map"))
        return std::move(Err);
      if (llvm::Error Err =
              ValidateRange(Layout->Try->Range, "C++ EH4 try map"))
        return std::move(Err);
      if (llvm::Error Err =
              ValidateRange(Layout->States->Range, "C++ EH4 IP map"))
        return std::move(Err);
      for (const coff_fh4::TryEntry &Try : Layout->Try->Entries) {
        if (llvm::Error Err =
                ValidateRange(Try.Handlers.Range, "C++ EH4 handler map"))
          return std::move(Err);
        for (const coff_fh4::HandlerEntry &Handler : Try.Handlers.Entries) {
          if (!PhysicalSemanticRows
                   .emplace(COFFGeneratedEHSemanticKind::CxxCatch, Entry.Begin,
                            Try.Range.BeginRVA, Handler.Range.BeginRVA)
                   .second)
            return patchError(
                "generated C++ EH4 handler map has duplicate rows");
        }
      }
      continue;
    }

    if (Plan.Model != COFFGeneratedLanguageModel::CxxFH3)
      return patchError("generated C++ owner has an unknown language model");

    const uint8_t *FuncInfo =
        ReadAtRVA(Plan.LanguageGroupRVA, FH3FuncInfoSize);
    if (!FuncInfo)
      return patchError("generated C++ FuncInfo is truncated");
    const int32_t MaxState = readLE<int32_t>(FuncInfo + sizeof(uint32_t));
    const uint32_t UnwindMapRVA =
        readLE<uint32_t>(FuncInfo + 2 * sizeof(uint32_t));
    const uint32_t NumTryBlocks =
        readLE<uint32_t>(FuncInfo + 3 * sizeof(uint32_t));
    const uint32_t TryMapRVA =
        readLE<uint32_t>(FuncInfo + 4 * sizeof(uint32_t));
    const uint32_t NumIPStates =
        readLE<uint32_t>(FuncInfo + 5 * sizeof(uint32_t));
    const uint32_t IPMapRVA = readLE<uint32_t>(FuncInfo + 6 * sizeof(uint32_t));
    if (MaxState < 0 ||
        static_cast<uint32_t>(MaxState) > MaxGeneratedLanguageRecords ||
        NumTryBlocks == 0 || NumTryBlocks > MaxGeneratedLanguageRecords ||
        NumIPStates == 0 || NumIPStates > MaxGeneratedLanguageRecords ||
        uint64_t(TryMapRVA) + uint64_t(NumTryBlocks) * FH3TryBlockSize >
            std::numeric_limits<uint32_t>::max())
      return patchError("generated C++ try map has an invalid extent");
    auto ValidateStateMap = [&](uint32_t RVA, size_t Count, size_t Stride,
                                const llvm::Twine &Description) -> llvm::Error {
      auto Owner = FindGeneratedSection(RVA, Count * Stride,
                                        /*RequireXData=*/true, Description);
      if (!Owner)
        return Owner.takeError();
      if ((*Owner)->Kind != llvm::mc_rewrite::RewriteSectionKind::ReadOnlyData)
        return patchError("generated " + Description +
                          " has invalid section permissions");
      return llvm::Error::success();
    };
    if (MaxState != 0)
      if (llvm::Error Err =
              ValidateStateMap(UnwindMapRVA, static_cast<uint32_t>(MaxState),
                               FH3StateMapEntrySize, "C++ unwind map"))
        return std::move(Err);
    if (llvm::Error Err = ValidateStateMap(TryMapRVA, NumTryBlocks,
                                           FH3TryBlockSize, "C++ try map"))
      return std::move(Err);
    if (llvm::Error Err = ValidateStateMap(
            IPMapRVA, NumIPStates, FH3StateMapEntrySize, "C++ IP map"))
      return std::move(Err);
    for (uint32_t TryIndex = 0; TryIndex < NumTryBlocks; ++TryIndex) {
      const uint32_t TryRVA = static_cast<uint32_t>(
          uint64_t(TryMapRVA) + uint64_t(TryIndex) * FH3TryBlockSize);
      const uint8_t *Try = ReadAtRVA(TryRVA, FH3TryBlockSize);
      if (!Try)
        return patchError("generated C++ try map is truncated");
      const uint32_t NumCatches = readLE<uint32_t>(Try + 3 * sizeof(uint32_t));
      const uint32_t HandlerMapRVA =
          readLE<uint32_t>(Try + 4 * sizeof(uint32_t));
      if (NumCatches == 0 || NumCatches > MaxGeneratedLanguageRecords)
        return patchError("generated C++ handler map has an invalid row count");
      if (llvm::Error Err =
              ValidateStateMap(HandlerMapRVA, NumCatches, FH3HandlerTypeSize,
                               "C++ handler map"))
        return std::move(Err);
      if (llvm::Error Err = AddPhysicalRows(
              COFFGeneratedEHSemanticKind::CxxCatch, Entry.Begin, TryRVA,
              HandlerMapRVA, NumCatches, FH3HandlerTypeSize))
        return std::move(Err);
    }
  }

  std::map<SemanticTokenIdentity, size_t> ObservedSemanticTokens;
  std::set<SemanticRowIdentity> BoundSemanticRows;
  auto encodeSEHCodeRVA = [&](uint32_t RVA) {
    return TargetArch == Arch::ARM && RVA != 0 ? RVA | uint32_t(1) : RVA;
  };
  for (const CompiledWinEHSemanticRecord &Record :
       Compiled.WinEHSemanticRecords) {
    const auto SourceOriginal =
        Compiled.SourceFunctionOriginalVAs.find(Record.SourceFunction);
    if (SourceOriginal == Compiled.SourceFunctionOriginalVAs.end())
      return patchError(
          "compiler EH semantic row has no source-function identity");
    const auto GeneratedOwner =
        GeneratedOwnerBySource.find(SourceOriginal->second);
    if (GeneratedOwner == GeneratedOwnerBySource.end() ||
        GeneratedOwner->second != Record.OwnerVA)
      return patchError(
          "compiler EH semantic row has a mismatched generated root owner");
    const ExceptionFunction *SourceEH = FindSourceEH(SourceOriginal->second);
    if (!SourceEH)
      return patchError(
          "compiler EH semantic row has no unique source exception record");

    std::optional<SemanticToken> ExpectedToken;
    if (Record.Token.Kind == SemanticKind::SEHScope)
      ExpectedToken = windows_eh_semantics::getSEHScopeSemanticToken(
          *SourceEH, TargetArch, Record.Token.Region);
    else if (Record.Token.Kind == SemanticKind::CxxCatch)
      ExpectedToken = windows_eh_semantics::getCxxCatchSemanticToken(
          *SourceEH, TargetArch, Record.Token.Region, Record.Token.Clause);
    else
      return patchError("compiler EH semantic row has an unknown token kind");
    if (!ExpectedToken || *ExpectedToken != Record.Token)
      return patchError(
          "compiler EH semantic row does not match its immutable source");

    const std::optional<uint32_t> OwnerRVA = ToRVA(Record.OwnerVA);
    const std::optional<uint32_t> ContainerRVA = ToRVA(Record.ContainerVA);
    const std::optional<uint32_t> RecordRVA = ToRVA(Record.RecordVA);
    const std::optional<uint32_t> HandlerRVA = ToRVA(Record.HandlerVA);
    if (!OwnerRVA || !ContainerRVA || !RecordRVA || !HandlerRVA)
      return patchError("compiler EH semantic row exceeds PE RVA limits");
    auto RecordOwner =
        FindGeneratedSection(*RecordRVA, Record.RecordSize,
                             /*RequireXData=*/true, "EH semantic row");
    auto HandlerOwner = FindGeneratedSection(
        *HandlerRVA, /*Size=*/1, /*RequireXData=*/false, "EH semantic handler");
    if (!RecordOwner)
      return RecordOwner.takeError();
    if (!HandlerOwner)
      return HandlerOwner.takeError();
    if ((*RecordOwner)->Kind !=
            llvm::mc_rewrite::RewriteSectionKind::ReadOnlyData ||
        (*HandlerOwner)->Kind != llvm::mc_rewrite::RewriteSectionKind::Code)
      return patchError(
          "compiler EH semantic row has invalid section permissions");

    const PlannedGeneratedLanguageOwner *LanguagePlan = nullptr;
    for (size_t I = 0; I < GeneratedEntries.size(); ++I) {
      if (!GeneratedLanguagePlans[I].Assigned ||
          GeneratedLanguagePlans[I].Role !=
              COFFGeneratedLanguageOwnerRole::MappedRoot ||
          GeneratedEntries[I].Begin != *OwnerRVA)
        continue;
      if (LanguagePlan)
        return patchError(
            "compiler EH semantic row has ambiguous language owners");
      LanguagePlan = &GeneratedLanguagePlans[I];
    }
    if (!LanguagePlan)
      return patchError(
          "compiler EH semantic row has no mapped language owner");

    const uint8_t *Bytes = ReadAtRVA(*RecordRVA, Record.RecordSize);
    if (!Bytes)
      return patchError("compiler EH semantic row is truncated");

    COFFGeneratedEHSemanticBinding Binding;
    Binding.Model = LanguagePlan->Model;
    Binding.SourceFunctionVA = SourceOriginal->second;
    Binding.Region = Record.Token.Region;
    Binding.Clause = Record.Token.Clause;
    Binding.SourceDigest = Record.Token.Digest;
    Binding.GeneratedOwnerRVA = *OwnerRVA;
    Binding.ContainerRVA = *ContainerRVA;
    Binding.RecordRVA = *RecordRVA;
    Binding.RecordBytes.assign(Bytes, Bytes + Record.RecordSize);
    Binding.HandlerRVA = *HandlerRVA;

    if (Record.Token.Kind == SemanticKind::SEHScope) {
      if (!SourceEH->SEH || Record.Token.Clause != 0 ||
          Record.Token.Region >= SourceEH->SEH->Scopes.size() ||
          Record.Encoding !=
              llvm::mc_rewrite::RewriteWinEHSemanticEncoding::SEH ||
          Binding.Model != COFFGeneratedLanguageModel::SEH ||
          Record.RecordSize != 4 * sizeof(uint32_t))
        return patchError("compiler SEH semantic row has an invalid shape");
      const std::optional<uint32_t> BeginRVA = ToRVA(Record.BeginVA);
      const std::optional<uint32_t> EndRVA = ToRVA(Record.EndVA);
      if (!BeginRVA || !EndRVA || *BeginRVA >= *EndRVA)
        return patchError("compiler SEH semantic range is invalid");
      Binding.Kind = COFFGeneratedEHSemanticKind::SEHScope;
      Binding.BeginRVA = *BeginRVA;
      Binding.EndRVA = *EndRVA;
      std::array<uint32_t, 4> Words{};
      for (size_t Word = 0; Word < Words.size(); ++Word)
        Words[Word] = readLE<uint32_t>(Bytes + Word * sizeof(uint32_t));
      const SEHScopeRecord &Scope = SourceEH->SEH->Scopes[Record.Token.Region];
      uint32_t ExpectedAction = 0;
      if (Scope.Kind == SEHScopeKind::Finally)
        ExpectedAction = encodeSEHCodeRVA(*HandlerRVA);
      else if (Scope.Kind == SEHScopeKind::CatchAll)
        ExpectedAction = 1;
      else {
        const std::optional<uint32_t> FilterRVA =
            ToRVA(Scope.FilterOrFinallyVA);
        if (!FilterRVA)
          return patchError("source SEH filter exceeds PE RVA limits");
        ExpectedAction = encodeSEHCodeRVA(*FilterRVA);
      }
      if (Words[0] != encodeSEHCodeRVA(*BeginRVA) ||
          Words[1] != encodeSEHCodeRVA(*EndRVA) || Words[2] != ExpectedAction ||
          Words[3] != (Scope.Kind == SEHScopeKind::Finally
                           ? 0
                           : encodeSEHCodeRVA(*HandlerRVA)))
        return patchError(
            "compiler SEH row fields do not match their source token");
    } else {
      if (!SourceEH->Cxx ||
          Record.Token.Region >= SourceEH->Cxx->TryBlocks.size() ||
          Record.Token.Clause >=
              SourceEH->Cxx->TryBlocks[Record.Token.Region].Handlers.size() ||
          !Record.BeginSymbol.empty() || !Record.EndSymbol.empty())
        return patchError("compiler C++ semantic row has an invalid shape");
      Binding.Kind = COFFGeneratedEHSemanticKind::CxxCatch;
      const CxxCatchHandler &Catch =
          SourceEH->Cxx->TryBlocks[Record.Token.Region]
              .Handlers[Record.Token.Clause];
      const std::optional<uint32_t> TypeRVA =
          Catch.TypeDescriptorVA == 0 ? std::optional<uint32_t>(0)
                                      : ToRVA(Catch.TypeDescriptorVA);
      if (!TypeRVA)
        return patchError("source C++ type exceeds PE RVA limits");

      if (SourceEH->Cxx->NativeEncoding == CxxExceptionInfo::Encoding::FH3) {
        if (Binding.Model != COFFGeneratedLanguageModel::CxxFH3 ||
            Record.Encoding !=
                llvm::mc_rewrite::RewriteWinEHSemanticEncoding::CxxFH3 ||
            Record.RecordSize != 5 * sizeof(uint32_t))
          return patchError("compiler C++ FH3 row has an invalid shape");
        std::array<uint32_t, 5> Words{};
        for (size_t Word = 0; Word < Words.size(); ++Word)
          Words[Word] = readLE<uint32_t>(Bytes + Word * sizeof(uint32_t));
        // The parent-frame offset is ABI-derived from the regenerated funclet
        // frame, so its source value is not layout-invariant. Authenticate the
        // compiler-emitted value through the receipt and constrain its
        // target ABI shape here. x64 homes the parent frame in the funclet
        // frame; AArch64 passes it through the funclet ABI and encodes zero.
        // The final gate rereads the same raw bytes.
        const uint32_t GeneratedParentFrameOffset = Words[4];
        if (Words[0] != Catch.Adjectives || Words[1] != *TypeRVA ||
            Words[2] != static_cast<uint32_t>(Catch.CatchObjectOffset) ||
            Words[3] != *HandlerRVA)
          return patchError(
              "compiler C++ FH3 row fields do not match their source token");
        const bool HasValidParentFrameOffset =
            TargetArch == Arch::AArch64
                ? GeneratedParentFrameOffset == 0
                : GeneratedParentFrameOffset != 0 &&
                      GeneratedParentFrameOffset <=
                          static_cast<uint32_t>(
                              std::numeric_limits<int32_t>::max()) &&
                      (GeneratedParentFrameOffset & (sizeof(uint64_t) - 1)) ==
                          0;
        if (!HasValidParentFrameOffset)
          return patchError(
              "compiler C++ FH3 parent-frame offset has an invalid ABI "
              "shape: 0x" +
              llvm::utohexstr(GeneratedParentFrameOffset));
      } else if (SourceEH->Cxx->NativeEncoding ==
                 CxxExceptionInfo::Encoding::FH4) {
        if (Binding.Model != COFFGeneratedLanguageModel::CxxFH4 ||
            Record.Encoding !=
                llvm::mc_rewrite::RewriteWinEHSemanticEncoding::CxxFH4 ||
            LanguagePlan->LanguageGroupRVA == 0)
          return patchError("compiler C++ EH4 row has an invalid shape");
        auto Layout =
            parseGeneratedFH4Layout(LanguagePlan->LanguageGroupRVA, ReadAtRVA);
        if (!Layout)
          return Layout.takeError();
        if (llvm::Error Err = validateBoundedGeneratedFH4Layout(*Layout))
          return std::move(Err);
        const coff_fh4::TryEntry &GeneratedTry = Layout->Try->Entries.front();
        const coff_fh4::HandlerEntry &GeneratedCatch =
            GeneratedTry.Handlers.Entries.front();
        if (Record.Token.Region != 0 || Record.Token.Clause != 0 ||
            GeneratedTry.Range.BeginRVA != *ContainerRVA ||
            GeneratedCatch.Range.BeginRVA != *RecordRVA ||
            GeneratedCatch.Range.size() != Record.RecordSize ||
            GeneratedCatch.Adjectives != Catch.Adjectives ||
            GeneratedCatch.TypeDescriptorRVA != *TypeRVA ||
            GeneratedCatch.CatchObjectOffset !=
                static_cast<uint32_t>(Catch.CatchObjectOffset) ||
            GeneratedCatch.HandlerRVA != *HandlerRVA ||
            !Catch.ContinuationVAs.empty())
          return patchError(
              "compiler C++ EH4 row fields do not match their source token");
      } else {
        return patchError("compiler C++ row has an unknown native encoding");
      }
    }

    ++ObservedSemanticTokens[TokenIdentity(Record.Token)];
    if (!BoundSemanticRows
             .emplace(Binding.Kind, Binding.GeneratedOwnerRVA,
                      Binding.ContainerRVA, Binding.RecordRVA)
             .second)
      return patchError("compiler EH semantic row is bound more than once");
    Update.GeneratedEHSemantics.push_back(std::move(Binding));
  }

  for (const auto &[Token, MinimumCount] : RequiredSemanticTokens) {
    const size_t Observed = ObservedSemanticTokens[Token];
    const bool IsCxx =
        Token[0] == static_cast<uint64_t>(SemanticKind::CxxCatch);
    if (Observed < MinimumCount || (IsCxx && Observed != MinimumCount))
      return patchError(
          "compiler EH semantic tokens do not close the immutable source "
          "graph");
  }
  if (ObservedSemanticTokens.size() != RequiredSemanticTokens.size() ||
      BoundSemanticRows != PhysicalSemanticRows)
    return patchError(
        "compiler EH semantic bindings do not close the generated language "
        "tables");
  std::sort(Update.GeneratedEHSemantics.begin(),
            Update.GeneratedEHSemantics.end(),
            [](const COFFGeneratedEHSemanticBinding &A,
               const COFFGeneratedEHSemanticBinding &B) {
              return A.RecordRVA < B.RecordRVA;
            });
  for (size_t I = 1; I < Update.GeneratedEHSemantics.size(); ++I) {
    const COFFGeneratedEHSemanticBinding &Previous =
        Update.GeneratedEHSemantics[I - 1];
    const COFFGeneratedEHSemanticBinding &Current =
        Update.GeneratedEHSemantics[I];
    if (Current.RecordRVA <
        uint64_t(Previous.RecordRVA) + Previous.RecordBytes.size())
      return patchError("generated EH semantic bindings overlap");
  }

  std::sort(Entries.begin(), Entries.end(),
            [](const RuntimeEntry &A, const RuntimeEntry &B) {
              return std::tie(A.Begin, A.End, A.Words) <
                     std::tie(B.Begin, B.End, B.Words);
            });
  Entries.erase(std::unique(Entries.begin(), Entries.end()), Entries.end());

  uint64_t TableOffset = alignUp(Compiled.Bytes.size(), uint64_t(4));
  uint64_t TableBytes = uint64_t(Entries.size()) * RuntimeFunctionSize;
  if (TableOffset > std::numeric_limits<size_t>::max() ||
      TableBytes > std::numeric_limits<size_t>::max() - TableOffset)
    return patchError("replacement exception directory exceeds host limits");
  if (TableOffset > std::numeric_limits<uint32_t>::max() - NewSectionRVA ||
      TableBytes >
          std::numeric_limits<uint32_t>::max() - (NewSectionRVA + TableOffset))
    return patchError("replacement exception directory exceeds PE RVA limits");
  uint64_t FinalVirtualSize = NewSectionRVA + TableOffset + TableBytes;
  if (llvm::Error Err =
          validateRuntimeEntries(Entries, FinalVirtualSize, TargetArch))
    return std::move(Err);

  Update.Apply = true;
  if (Entries.empty()) {
    Update.RVA = 0;
    Update.Size = 0;
    return Update;
  }

  Compiled.Bytes.resize(static_cast<size_t>(TableOffset + TableBytes), 0);
  uint8_t *Out = Compiled.Bytes.data() + TableOffset;
  for (const RuntimeEntry &Entry : Entries) {
    if (Entry.WordCount != RuntimeFunctionSize / 4)
      return patchError("runtime-function serialization width changed");
    for (uint8_t I = 0; I < Entry.WordCount; ++I)
      writeLE<uint32_t>(Out + uint64_t(I) * 4, Entry.Words[I]);
    Out += RuntimeFunctionSize;
  }
  Compiled.Sections.push_back(
      {".ndpdata", TableOffset, NewSectionVA + TableOffset, TableBytes, 4,
       llvm::mc_rewrite::RewriteSectionKind::ReadOnlyData, true});
  Update.RVA = static_cast<uint32_t>(NewSectionRVA + TableOffset);
  Update.Size = static_cast<uint32_t>(TableBytes);

  std::set<size_t> ReceiptedFinalRows;
  for (size_t I = 0; I < GeneratedEntries.size(); ++I) {
    const PlannedGeneratedLanguageOwner &Plan = GeneratedLanguagePlans[I];
    if (!Plan.Assigned)
      continue;
    std::optional<size_t> FinalRow;
    for (size_t J = 0; J < Entries.size(); ++J) {
      if (!(Entries[J] == GeneratedEntries[I]))
        continue;
      if (FinalRow)
        return patchError(
            "generated language owner has ambiguous final directory rows");
      FinalRow = J;
    }
    if (!FinalRow || !ReceiptedFinalRows.insert(*FinalRow).second)
      return patchError(
          "generated language owner has no unique final directory row");

    const RuntimeEntry &Entry = Entries[*FinalRow];
    const uint64_t RuntimeFunctionRVA =
        uint64_t(Update.RVA) + uint64_t(*FinalRow) * RuntimeFunctionSize;
    if (RuntimeFunctionRVA > std::numeric_limits<uint32_t>::max())
      return patchError(
          "generated language owner directory location exceeds PE limits");
    COFFGeneratedLanguageOwnerReceipt Receipt;
    Receipt.RuntimeFunctionRVA = static_cast<uint32_t>(RuntimeFunctionRVA);
    Receipt.RuntimeWords = Entry.Words;
    Receipt.RuntimeWordCount = Entry.WordCount;
    Receipt.BeginRVA = Entry.Begin;
    Receipt.EndRVA = Entry.End;
    Receipt.UnwindRVA = Entry.Unwind;
    Receipt.HandlerRVA = Entry.Handler;
    Receipt.HandlerDataRVA = Entry.HandlerData;
    Receipt.LanguageGroupRVA = Plan.LanguageGroupRVA;
    Receipt.GSWrapped = Plan.GSWrapped;
    Receipt.GSCookieHeader = Plan.GSCookieHeader;
    Receipt.Model = Plan.Model;
    Receipt.Role = Plan.Role;
    Update.GeneratedLanguageOwners.push_back(std::move(Receipt));
  }
  std::sort(Update.GeneratedLanguageOwners.begin(),
            Update.GeneratedLanguageOwners.end(),
            [](const COFFGeneratedLanguageOwnerReceipt &A,
               const COFFGeneratedLanguageOwnerReceipt &B) {
              return A.RuntimeFunctionRVA < B.RuntimeFunctionRVA;
            });

  for (const CompiledSection &Section : Compiled.Sections) {
    if (!Section.IsAllocated || !Section.IsInImage || Section.Size == 0)
      continue;
    const uint64_t SectionRVA = Section.VA - Image.Base;
    if (Section.Offset > InvalidVA - NewSectionVA ||
        Section.VA != NewSectionVA + Section.Offset ||
        Section.VA < Image.Base || Section.Size > InvalidVA - Section.VA ||
        SectionRVA > std::numeric_limits<uint32_t>::max() ||
        Section.Size > std::numeric_limits<uint32_t>::max() ||
        Section.Size > uint64_t(std::numeric_limits<uint32_t>::max()) + 1 -
                           SectionRVA ||
        !rangeInBounds(Section.Offset, Section.Size, Compiled.Bytes.size()))
      return patchError(
          "generated section receipt has an invalid placement or extent");
    COFFGeneratedSectionReceipt Receipt;
    Receipt.RVA = static_cast<uint32_t>(Section.VA - Image.Base);
    Receipt.Size = static_cast<uint32_t>(Section.Size);
    Receipt.SHA256 = llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(
        Compiled.Bytes.data() + static_cast<size_t>(Section.Offset),
        static_cast<size_t>(Section.Size)));
    Update.GeneratedSections.push_back(std::move(Receipt));
  }
  std::sort(Update.GeneratedSections.begin(), Update.GeneratedSections.end(),
            [](const COFFGeneratedSectionReceipt &A,
               const COFFGeneratedSectionReceipt &B) {
              return std::tie(A.RVA, A.Size) < std::tie(B.RVA, B.Size);
            });
  for (size_t I = 1; I < Update.GeneratedSections.size(); ++I) {
    const COFFGeneratedSectionReceipt &Previous =
        Update.GeneratedSections[I - 1];
    const COFFGeneratedSectionReceipt &Current = Update.GeneratedSections[I];
    if (Current.RVA < uint64_t(Previous.RVA) + Previous.Size)
      return patchError("generated section receipts overlap");
  }

  auto IsCoveredByGeneratedReceipt = [&](uint32_t RVA, size_t Size) {
    return std::any_of(
        Update.GeneratedSections.begin(), Update.GeneratedSections.end(),
        [&](const COFFGeneratedSectionReceipt &Section) {
          return RVA >= Section.RVA &&
                 uint64_t(RVA) + Size <= uint64_t(Section.RVA) + Section.Size;
        });
  };
  for (const COFFGeneratedLanguageOwnerReceipt &Receipt :
       Update.GeneratedLanguageOwners) {
    if (!IsCoveredByGeneratedReceipt(Receipt.UnwindRVA, sizeof(uint32_t)) ||
        !IsCoveredByGeneratedReceipt(
            Receipt.HandlerDataRVA,
            Receipt.GSWrapped ? 2 * sizeof(uint32_t) : sizeof(uint32_t)) ||
        (Receipt.LanguageGroupRVA != 0 &&
         !IsCoveredByGeneratedReceipt(Receipt.LanguageGroupRVA,
                                      Receipt.Model ==
                                              COFFGeneratedLanguageModel::CxxFH3
                                          ? FH3FuncInfoSize
                                          : 1)))
      return patchError(
          "generated language receipt references unsigned section data");
  }
  return Update;
}

llvm::Error
applyCOFFExceptionDirectoryUpdate(std::vector<uint8_t> &Binary,
                                  const COFFExceptionDirectoryUpdate &Update) {
  if (!Update.Apply)
    return llvm::Error::success();
  auto PE = locatePEHeaders(Binary.data(), Binary.size());
  if (!PE.valid())
    return patchError("patched image has invalid PE headers");
  if (!getPEDataDirectory(PE, llvm::COFF::EXCEPTION_TABLE))
    return patchError("patched image has no exception-directory slot");
  setPEDataDirectory(PE, llvm::COFF::EXCEPTION_TABLE, Update.RVA, Update.Size);
  clearPEChecksum(PE);
  return llvm::Error::success();
}

llvm::Expected<COFFGuardTableUpdate> prepareCOFFGuardTables(
    llvm::ArrayRef<uint8_t> OriginalBinary, const BinaryImage &Image,
    CompiledImage &Compiled,
    llvm::ArrayRef<std::pair<va_t, va_t>> PatchedEntryMappings,
    uint64_t NewSectionVA, Arch TargetArch,
    bool RequireGeneratedEHContinuations) {
  COFFGuardTableUpdate Update;
  if (OriginalBinary.empty())
    return patchError("empty input image while rebuilding guard tables");
  auto PE = locatePEHeaders(const_cast<uint8_t *>(OriginalBinary.data()),
                            OriginalBinary.size());
  if (!PE.valid())
    return patchError("invalid PE headers while rebuilding guard tables");
  if (NewSectionVA < Image.Base ||
      NewSectionVA - Image.Base > std::numeric_limits<uint32_t>::max())
    return patchError("guard table placement exceeds PE RVA limits");
  if ((TargetArch == Arch::X64 || TargetArch == Arch::AArch64) != PE.Is64)
    return patchError(
        "guard table PE class does not match target architecture");

  const uint32_t GuardFlags = Image.DynInfo.GuardFlags;
  const uint32_t CFPresent =
      uint32_t(llvm::COFF::GuardFlags::CF_FUNCTION_TABLE_PRESENT);
  const uint32_t EHPresent =
      uint32_t(llvm::COFF::GuardFlags::EH_CONTINUATION_TABLE_PRESENT);
  const bool HasCF = (GuardFlags & CFPresent) != 0;
  const bool HasEHCont = (GuardFlags & EHPresent) != 0;

  // These modes require code-generation contracts beyond ordinary CFG/EHCont
  // tables (write guard, return-flow guard, retpolines, or XFG hashes).  Keep
  // their existing metadata intact for analysis, but never advertise them on
  // newly generated code until the corresponding backend contracts are
  // represented and verified.
  if (!PatchedEntryMappings.empty() &&
      hasUnsupportedGeneratedGuardMode(GuardFlags))
    return patchError(
        "input uses a guard instrumentation mode not supported by generated "
        "code");
  if (!HasCF && !HasEHCont)
    return Update;

  if (HasCF) {
    for (const std::string &Unresolved : Compiled.Unresolved) {
      llvm::StringRef Name(Unresolved);
      Name.consume_front("\01");
      Name.consume_front("__imp_");
      if (Name == "__guard_check_icall_fptr" ||
          Name == "__guard_dispatch_icall_fptr" ||
          Name == "__guard_xfg_check_icall_fptr" ||
          Name == "__guard_xfg_dispatch_icall_fptr")
        return patchError("generated CFG instrumentation has an unresolved " +
                          llvm::Twine(Unresolved));
    }
  }

  const bool HasGeneratedCFReferences = std::any_of(
      Compiled.Sections.begin(), Compiled.Sections.end(),
      [](const CompiledSection &Section) {
        return llvm::StringRef(Section.Name).starts_with(".gfids") &&
               !Section.SymbolIndexReferences.empty();
      });

  const uint64_t NewSectionRVA = NewSectionVA - Image.Base;
  auto ToRVA = [&](va_t VA) -> llvm::Expected<uint32_t> {
    if (VA < Image.Base ||
        VA - Image.Base > std::numeric_limits<uint32_t>::max())
      return patchError("generated guard target exceeds PE RVA limits");
    return static_cast<uint32_t>(VA - Image.Base);
  };
  auto IsOriginalExecutableRVA = [&](uint32_t RVA) {
    if (RVA >= getPESizeOfImage(PE) || RVA > InvalidVA - Image.Base)
      return false;
    const Segment *Segment = Image.getSegmentFor(Image.Base + RVA);
    return Segment && Segment->isExecutable();
  };
  auto IsGeneratedCodeVA = [&](va_t VA) {
    for (const CompiledSection &Section : Compiled.Sections) {
      if (!Section.IsAllocated ||
          Section.Kind != llvm::mc_rewrite::RewriteSectionKind::Code ||
          VA < Section.VA)
        continue;
      if (VA - Section.VA < Section.Size)
        return true;
    }
    return false;
  };
  auto IsGuardCodeVA = [&](va_t VA) {
    if (IsGeneratedCodeVA(VA))
      return true;
    if (VA < Image.Base ||
        VA - Image.Base > std::numeric_limits<uint32_t>::max())
      return false;
    return IsOriginalExecutableRVA(static_cast<uint32_t>(VA - Image.Base));
  };
  auto AppendTable =
      [&](llvm::StringRef Name,
          llvm::ArrayRef<uint8_t> Bytes) -> llvm::Expected<uint32_t> {
    uint64_t Offset = alignUp(Compiled.Bytes.size(), uint64_t(4));
    if (Offset > std::numeric_limits<size_t>::max() ||
        Bytes.size() > std::numeric_limits<size_t>::max() - Offset ||
        Offset > std::numeric_limits<uint32_t>::max() - NewSectionRVA ||
        Bytes.size() >
            std::numeric_limits<uint32_t>::max() - (NewSectionRVA + Offset))
      return patchError("replacement guard table exceeds PE RVA limits");
    Compiled.Bytes.resize(static_cast<size_t>(Offset), 0);
    Compiled.Bytes.insert(Compiled.Bytes.end(), Bytes.begin(), Bytes.end());
    Compiled.Sections.push_back(
        {Name.str(), Offset, NewSectionVA + Offset,
         static_cast<uint64_t>(Bytes.size()), 4,
         llvm::mc_rewrite::RewriteSectionKind::ReadOnlyData, true});
    return static_cast<uint32_t>(NewSectionRVA + Offset);
  };

  if (HasCF && (!PatchedEntryMappings.empty() || HasGeneratedCFReferences)) {
    uint32_t ExtraBytes =
        (GuardFlags &
         uint32_t(llvm::COFF::GuardFlags::CF_FUNCTION_TABLE_SIZE_MASK)) >>
        28;
    uint32_t Stride = 4 + ExtraBytes;
    std::map<uint32_t, std::vector<uint8_t>> Entries;
    uint64_t OriginalCount = Image.DynInfo.GuardCFFunctionCount;
    if (OriginalCount != 0) {
      if (Image.DynInfo.GuardCFFunctionTableRVA == 0 ||
          Image.DynInfo.GuardCFFunctionTableRVA >
              std::numeric_limits<uint32_t>::max() ||
          OriginalCount > OriginalBinary.size() / Stride)
        return patchError("invalid Guard CF function-table extent");
      uint64_t Bytes = OriginalCount * Stride;
      auto Offset = rvaToFileOffset(
          PE, OriginalBinary,
          static_cast<uint32_t>(Image.DynInfo.GuardCFFunctionTableRVA), Bytes);
      if (!Offset)
        return patchError("Guard CF function table is not file-backed");
      for (uint64_t I = 0; I < OriginalCount; ++I) {
        const uint8_t *Record = OriginalBinary.data() + *Offset + I * Stride;
        uint32_t RVA = readLE<uint32_t>(Record);
        if (!IsOriginalExecutableRVA(RVA))
          return patchError("Guard CF table contains a non-executable target");
        Entries.emplace(RVA, std::vector<uint8_t>(Record, Record + Stride));
      }
    }
    for (const CompiledSection &Section : Compiled.Sections) {
      if (!llvm::StringRef(Section.Name).starts_with(".gfids"))
        continue;
      for (const auto &Reference : Section.SymbolIndexReferences) {
        if (!rangeInBounds(Reference.Offset, sizeof(uint32_t), Section.Size))
          return patchError("generated Guard CF record is truncated");
        if (Reference.TargetVA == 0 || !IsGuardCodeVA(Reference.TargetVA))
          return patchError("generated Guard CF reference is not code");
        auto RVAOrErr = ToRVA(Reference.TargetVA);
        if (!RVAOrErr)
          return RVAOrErr.takeError();
        std::vector<uint8_t> Record(Stride, 0);
        writeLE<uint32_t>(Record.data(), *RVAOrErr);
        Entries.emplace(*RVAOrErr, std::move(Record));
      }
    }
    for (const auto &[OriginalVA, GeneratedVA] : PatchedEntryMappings) {
      (void)OriginalVA;
      if (!IsGeneratedCodeVA(GeneratedVA))
        return patchError("generated Guard CF target is not emitted code");
      auto RVAOrErr = ToRVA(GeneratedVA);
      if (!RVAOrErr)
        return RVAOrErr.takeError();
      std::vector<uint8_t> Record(Stride, 0);
      writeLE<uint32_t>(Record.data(), *RVAOrErr);
      Entries.emplace(*RVAOrErr, std::move(Record));
    }
    std::vector<uint8_t> Bytes;
    Bytes.reserve(Entries.size() * Stride);
    for (const auto &[RVA, Record] : Entries) {
      (void)RVA;
      Bytes.insert(Bytes.end(), Record.begin(), Record.end());
    }
    auto RVAOrErr = AppendTable(".ndgfids", Bytes);
    if (!RVAOrErr)
      return RVAOrErr.takeError();
    Update.ApplyCF = true;
    Update.CFFunctionTableRVA = *RVAOrErr;
    Update.CFFunctionCount = Entries.size();
  }

  if (HasEHCont) {
    std::set<uint32_t> Entries;
    uint64_t OriginalCount = Image.DynInfo.GuardEHContinuationCount;
    if (OriginalCount != 0) {
      if (Image.DynInfo.GuardEHContinuationTableRVA == 0 ||
          Image.DynInfo.GuardEHContinuationTableRVA >
              std::numeric_limits<uint32_t>::max() ||
          OriginalCount > OriginalBinary.size() / sizeof(uint32_t))
        return patchError("invalid Guard EH continuation-table extent");
      uint64_t Bytes = OriginalCount * sizeof(uint32_t);
      auto Offset = rvaToFileOffset(
          PE, OriginalBinary,
          static_cast<uint32_t>(Image.DynInfo.GuardEHContinuationTableRVA),
          Bytes);
      if (!Offset)
        return patchError("Guard EH continuation table is not file-backed");
      for (uint64_t I = 0; I < OriginalCount; ++I) {
        uint32_t RVA = readLE<uint32_t>(OriginalBinary.data() + *Offset +
                                        I * sizeof(uint32_t));
        if (!IsOriginalExecutableRVA(RVA))
          return patchError(
              "Guard EH continuation table has a non-executable target");
        Entries.insert(RVA);
      }
    }

    size_t GeneratedCount = 0;
    for (const CompiledSection &Section : Compiled.Sections) {
      if (!llvm::StringRef(Section.Name).starts_with(".gehcont"))
        continue;
      for (const auto &Reference : Section.SymbolIndexReferences) {
        if (!rangeInBounds(Reference.Offset, sizeof(uint32_t), Section.Size))
          return patchError(
              "generated Guard EH continuation record is truncated");
        if (Reference.TargetVA == 0 || !IsGeneratedCodeVA(Reference.TargetVA))
          return patchError("generated Guard EH continuation is not code");
        auto RVAOrErr = ToRVA(Reference.TargetVA);
        if (!RVAOrErr)
          return RVAOrErr.takeError();
        Entries.insert(*RVAOrErr);
        ++GeneratedCount;
      }
    }
    if (RequireGeneratedEHContinuations && GeneratedCount == 0)
      return patchError(
          "native language EH emitted no Guard EH continuation targets");
    if (GeneratedCount != 0) {
      std::vector<uint8_t> Bytes(Entries.size() * sizeof(uint32_t));
      size_t I = 0;
      for (uint32_t RVA : Entries)
        writeLE<uint32_t>(Bytes.data() + I++ * sizeof(uint32_t), RVA);
      auto RVAOrErr = AppendTable(".ndgehcont", Bytes);
      if (!RVAOrErr)
        return RVAOrErr.takeError();
      Update.ApplyEHCont = true;
      Update.EHContinuationTableRVA = *RVAOrErr;
      Update.EHContinuationCount = Entries.size();
    }
  }
  return Update;
}

llvm::Error applyCOFFGuardTableUpdate(std::vector<uint8_t> &Binary,
                                      const BinaryImage &Image,
                                      const COFFGuardTableUpdate &Update) {
  if (!Update.ApplyCF && !Update.ApplyEHCont)
    return llvm::Error::success();
  auto PE = locatePEHeaders(Binary.data(), Binary.size());
  if (!PE.valid())
    return patchError("patched image has invalid PE headers for load config");
  if (Image.DynInfo.LoadConfigRVA == 0 ||
      Image.DynInfo.LoadConfigRVA > std::numeric_limits<uint32_t>::max() ||
      Image.DynInfo.LoadConfigSize == 0)
    return patchError("guard-enabled image has no writable load config");
  auto ConfigOffset = rvaToFileOffset(
      PE, Binary, static_cast<uint32_t>(Image.DynInfo.LoadConfigRVA),
      Image.DynInfo.LoadConfigSize);
  if (!ConfigOffset)
    return patchError("load configuration is not file-backed");

  auto WritePair = [&](size_t TableOffset, size_t CountOffset,
                       uint32_t TableRVA, uint64_t Count) -> llvm::Error {
    size_t Width = PE.Is64 ? sizeof(uint64_t) : sizeof(uint32_t);
    if (!rangeInBounds(TableOffset, Width, Image.DynInfo.LoadConfigSize) ||
        !rangeInBounds(CountOffset, Width, Image.DynInfo.LoadConfigSize) ||
        !rangeInBounds(*ConfigOffset + TableOffset, Width, Binary.size()) ||
        !rangeInBounds(*ConfigOffset + CountOffset, Width, Binary.size()))
      return patchError("load configuration is too short for guard fields");
    if (TableRVA > InvalidVA - Image.Base)
      return patchError("guard table VA overflows");
    uint64_t TableVA = Image.Base + TableRVA;
    if (PE.Is64) {
      writeLE<uint64_t>(Binary.data() + *ConfigOffset + TableOffset, TableVA);
      writeLE<uint64_t>(Binary.data() + *ConfigOffset + CountOffset, Count);
    } else {
      if (TableVA > std::numeric_limits<uint32_t>::max() ||
          Count > std::numeric_limits<uint32_t>::max())
        return patchError("32-bit guard load-config field overflows");
      writeLE<uint32_t>(Binary.data() + *ConfigOffset + TableOffset,
                        static_cast<uint32_t>(TableVA));
      writeLE<uint32_t>(Binary.data() + *ConfigOffset + CountOffset,
                        static_cast<uint32_t>(Count));
    }
    return llvm::Error::success();
  };

  using llvm::object::coff_load_configuration32;
  using llvm::object::coff_load_configuration64;
  if (Update.ApplyCF) {
    size_t TableOffset =
        PE.Is64 ? offsetof(coff_load_configuration64, GuardCFFunctionTable)
                : offsetof(coff_load_configuration32, GuardCFFunctionTable);
    size_t CountOffset =
        PE.Is64 ? offsetof(coff_load_configuration64, GuardCFFunctionCount)
                : offsetof(coff_load_configuration32, GuardCFFunctionCount);
    if (llvm::Error Err =
            WritePair(TableOffset, CountOffset, Update.CFFunctionTableRVA,
                      Update.CFFunctionCount))
      return Err;
  }
  if (Update.ApplyEHCont) {
    size_t TableOffset =
        PE.Is64 ? offsetof(coff_load_configuration64, GuardEHContinuationTable)
                : offsetof(coff_load_configuration32, GuardEHContinuationTable);
    size_t CountOffset =
        PE.Is64 ? offsetof(coff_load_configuration64, GuardEHContinuationCount)
                : offsetof(coff_load_configuration32, GuardEHContinuationCount);
    if (llvm::Error Err =
            WritePair(TableOffset, CountOffset, Update.EHContinuationTableRVA,
                      Update.EHContinuationCount))
      return Err;
  }
  clearPEChecksum(PE);
  return llvm::Error::success();
}

static llvm::Error validatePatchedCOFFImageImpl(
    llvm::ArrayRef<uint8_t> Binary, Arch TargetArch,
    bool RequireGeneratedExceptionDirectory,
    const COFFExceptionDirectoryUpdate *ExpectedExceptionDirectory) {
  if (Binary.empty())
    return patchError("final PE image is empty");
  if (ExpectedExceptionDirectory) {
    const bool HasReceipts =
        !ExpectedExceptionDirectory->GeneratedLanguageOwners.empty() ||
        !ExpectedExceptionDirectory->GeneratedSections.empty() ||
        !ExpectedExceptionDirectory->GeneratedEHSemantics.empty();
    if (!ExpectedExceptionDirectory->Apply &&
        (ExpectedExceptionDirectory->RVA != 0 ||
         ExpectedExceptionDirectory->Size != 0 || HasReceipts))
      return patchError(
          "non-applied exception update carries replacement state");
    if (ExpectedExceptionDirectory->Apply &&
        ExpectedExceptionDirectory->Size == 0 &&
        (ExpectedExceptionDirectory->RVA != 0 || HasReceipts))
      return patchError(
          "empty exception update carries generated replacement state");
    if (ExpectedExceptionDirectory->Apply &&
        ExpectedExceptionDirectory->Size != 0 &&
        ExpectedExceptionDirectory->RVA == 0)
      return patchError("prepared exception update has a zero RVA");
  }
  const bool HasExpectedReplacement = ExpectedExceptionDirectory &&
                                      ExpectedExceptionDirectory->Apply &&
                                      ExpectedExceptionDirectory->Size != 0;
  RequireGeneratedExceptionDirectory |= HasExpectedReplacement;
  llvm::MemoryBufferRef Buffer(
      llvm::StringRef(reinterpret_cast<const char *>(Binary.data()),
                      Binary.size()),
      "neverd-patched-pe");
  auto ObjectOrErr = llvm::object::ObjectFile::createObjectFile(Buffer);
  if (!ObjectOrErr)
    return patchError("LLVM rejected final PE image: " +
                      llvm::toString(ObjectOrErr.takeError()));
  const auto *COFF =
      llvm::dyn_cast<llvm::object::COFFObjectFile>(ObjectOrErr->get());
  if (!COFF)
    return patchError("final image is not a COFF object");
  uint16_t ExpectedMachine = 0;
  switch (TargetArch) {
  case Arch::X64:
    ExpectedMachine = llvm::COFF::IMAGE_FILE_MACHINE_AMD64;
    break;
  case Arch::AArch64:
    ExpectedMachine = llvm::COFF::IMAGE_FILE_MACHINE_ARM64;
    break;
  case Arch::ARM:
    ExpectedMachine = llvm::COFF::IMAGE_FILE_MACHINE_ARMNT;
    break;
  default:
    return patchError("final PE target architecture is unsupported");
  }
  if (COFF->getMachine() != ExpectedMachine)
    return patchError("final PE machine does not match target architecture");

  auto PE =
      locatePEHeaders(const_cast<uint8_t *>(Binary.data()), Binary.size());
  if (!PE.valid())
    return patchError("final PE headers are invalid");
  if (!PE.SectionTable || PE.NumSections == 0)
    return patchError("final PE has no valid section table");
  if ((TargetArch == Arch::X64 || TargetArch == Arch::AArch64) != PE.Is64)
    return patchError("final PE class does not match target architecture");
  uint64_t ImageBase = getPEImageBase(PE);
  uint32_t SizeOfImage = getPESizeOfImage(PE);
  uint32_t SizeOfHeaders = getPESizeOfHeaders(PE);
  if (ImageBase == 0 || SizeOfImage == 0 || SizeOfHeaders == 0 ||
      SizeOfHeaders > Binary.size() || SizeOfImage > InvalidVA - ImageBase)
    return patchError("final PE has an invalid image extent");

  std::vector<std::pair<uint64_t, uint64_t>> VirtualSectionRanges;
  std::vector<std::pair<uint64_t, uint64_t>> RawSectionRanges;
  bool InvalidSection = false;
  forEachPESection(PE, [&](const PESectionFields &Section, uint16_t) {
    uint64_t VirtualExtent =
        std::max<uint32_t>(Section.VirtualSize, Section.SizeOfRawData);
    if (VirtualExtent != 0) {
      if (Section.VirtualAddress >= SizeOfImage ||
          VirtualExtent > SizeOfImage - Section.VirtualAddress) {
        InvalidSection = true;
      } else {
        VirtualSectionRanges.emplace_back(Section.VirtualAddress,
                                          uint64_t(Section.VirtualAddress) +
                                              VirtualExtent);
      }
    }
    if (Section.SizeOfRawData != 0) {
      if (Section.PointerToRawData < SizeOfHeaders ||
          !rangeInBounds(Section.PointerToRawData, Section.SizeOfRawData,
                         Binary.size())) {
        InvalidSection = true;
      } else {
        RawSectionRanges.emplace_back(Section.PointerToRawData,
                                      uint64_t(Section.PointerToRawData) +
                                          Section.SizeOfRawData);
      }
    }
  });
  auto HasOverlap = [](auto &Ranges) {
    std::sort(Ranges.begin(), Ranges.end());
    for (size_t I = 1; I < Ranges.size(); ++I)
      if (Ranges[I].first < Ranges[I - 1].second)
        return true;
    return false;
  };
  if (InvalidSection || HasOverlap(VirtualSectionRanges) ||
      HasOverlap(RawSectionRanges))
    return patchError("final PE section extents are invalid or overlapping");

  BinaryImage ValidationImage;
  ValidationImage.Base = ImageBase;
  ValidationImage.Arch = TargetArch;
  if (TargetArch == Arch::ARM)
    ValidationImage.Mode = InstructionMode::Thumb;
  ValidationImage.Bits = PE.Is64 ? Bitness::Bits64 : Bitness::Bits32;
  ValidationImage.Format = BinaryFormat::COFF;
  forEachPESection(PE, [&](const PESectionFields &Section, uint16_t) {
    uint64_t VirtualExtent =
        std::max<uint32_t>(Section.VirtualSize, Section.SizeOfRawData);
    if (VirtualExtent == 0 || Section.VirtualAddress > InvalidVA - ImageBase)
      return;
    Segment Mapped;
    Mapped.VA = ImageBase + Section.VirtualAddress;
    Mapped.Size = VirtualExtent;
    Mapped.FileOff = Section.PointerToRawData;
    Mapped.FileSz = Section.SizeOfRawData;
    Mapped.Flags = coffFlagsToNd(Section.Characteristics);
    if (Section.SizeOfRawData != 0)
      Mapped.Data.assign(Binary.begin() + Section.PointerToRawData,
                         Binary.begin() + Section.PointerToRawData +
                             Section.SizeOfRawData);
    ValidationImage.Segments.push_back(std::move(Mapped));
  });

  auto ReadAtRVA = [&](uint32_t RVA, size_t Size) -> const uint8_t * {
    auto Offset = rvaToFileOffset(PE, Binary, RVA, Size);
    return Offset ? Binary.data() + *Offset : nullptr;
  };
  auto IsExecutableRVA = [&](uint32_t RVA) {
    if (RVA >= SizeOfImage)
      return false;
    bool Executable = false;
    forEachPESection(PE, [&](const PESectionFields &Section, uint16_t) {
      if (RVA < Section.VirtualAddress)
        return;
      uint64_t Delta = uint64_t(RVA) - Section.VirtualAddress;
      uint64_t Extent =
          std::max<uint32_t>(Section.VirtualSize, Section.SizeOfRawData);
      if (Delta < Extent &&
          (Section.Characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE) != 0)
        Executable = true;
    });
    return Executable;
  };

  const llvm::object::data_directory *ExceptionDirectory =
      getPEDataDirectory(PE, llvm::COFF::EXCEPTION_TABLE);
  if (ExpectedExceptionDirectory && ExpectedExceptionDirectory->Apply) {
    const uint32_t FinalRVA =
        ExceptionDirectory ? ExceptionDirectory->RelativeVirtualAddress : 0;
    const uint32_t FinalSize =
        ExceptionDirectory ? ExceptionDirectory->Size : 0;
    if (FinalRVA != ExpectedExceptionDirectory->RVA ||
        FinalSize != ExpectedExceptionDirectory->Size)
      return patchError(
          "final exception directory does not match the prepared update");
  }
  if (RequireGeneratedExceptionDirectory &&
      (!ExceptionDirectory || ExceptionDirectory->Size == 0))
    return patchError("final replacement exception directory is missing");
  if (ExceptionDirectory && ExceptionDirectory->Size != 0) {
    uint64_t RecordSize = TargetArch == Arch::X64 ? X64RuntimeFunctionSize
                                                  : ARMRuntimeFunctionSize;
    if (TargetArch != Arch::X64 && TargetArch != Arch::AArch64 &&
        TargetArch != Arch::ARM)
      return patchError("final exception table uses unsupported architecture");
    if (ExceptionDirectory->RelativeVirtualAddress == 0 ||
        ExceptionDirectory->Size % RecordSize != 0)
      return patchError("final exception directory has an invalid extent");
    if (ExceptionDirectory->RelativeVirtualAddress >= SizeOfImage ||
        ExceptionDirectory->Size >
            SizeOfImage - ExceptionDirectory->RelativeVirtualAddress)
      return patchError("final exception directory leaves the image extent");
    auto TableOffset =
        rvaToFileOffset(PE, Binary, ExceptionDirectory->RelativeVirtualAddress,
                        ExceptionDirectory->Size);
    if (!TableOffset)
      return patchError("final exception directory is not file-backed");

    std::optional<PESectionFields> GeneratedSection;
    forEachPESection(PE, [&](const PESectionFields &Section, uint16_t) {
      if (GeneratedSection || readCOFFName(Section.Name) != kNdTextSection ||
          ExceptionDirectory->RelativeVirtualAddress < Section.VirtualAddress)
        return;
      const uint64_t Delta =
          uint64_t(ExceptionDirectory->RelativeVirtualAddress) -
          Section.VirtualAddress;
      const uint64_t Extent =
          std::max<uint32_t>(Section.VirtualSize, Section.SizeOfRawData);
      if (Delta <= Extent && ExceptionDirectory->Size <= Extent - Delta)
        GeneratedSection = Section;
    });
    if (RequireGeneratedExceptionDirectory && !GeneratedSection)
      return patchError(
          "final replacement exception directory has no generated section "
          "owner");
    if (RequireGeneratedExceptionDirectory &&
        ((GeneratedSection->Characteristics & llvm::COFF::IMAGE_SCN_CNT_CODE) ==
             0 ||
         (GeneratedSection->Characteristics &
          llvm::COFF::IMAGE_SCN_MEM_EXECUTE) == 0 ||
         (GeneratedSection->Characteristics & llvm::COFF::IMAGE_SCN_MEM_READ) ==
             0))
      return patchError(
          "final replacement exception directory owner is not executable "
          "code");
    auto IsInGeneratedSection = [&](uint32_t RVA) {
      if (!GeneratedSection || RVA < GeneratedSection->VirtualAddress)
        return false;
      const uint64_t Delta = uint64_t(RVA) - GeneratedSection->VirtualAddress;
      const uint64_t Extent = std::max<uint32_t>(
          GeneratedSection->VirtualSize, GeneratedSection->SizeOfRawData);
      return Delta < Extent;
    };
    std::optional<uint32_t> GeneratedSectionEndRVA;
    if (GeneratedSection) {
      const uint64_t Extent = std::max<uint32_t>(
          GeneratedSection->VirtualSize, GeneratedSection->SizeOfRawData);
      const uint64_t End = uint64_t(GeneratedSection->VirtualAddress) + Extent;
      if (End <= std::numeric_limits<uint32_t>::max())
        GeneratedSectionEndRVA = static_cast<uint32_t>(End);
    }

    llvm::ArrayRef<COFFGeneratedLanguageOwnerReceipt> ExpectedOwners;
    llvm::ArrayRef<COFFGeneratedSectionReceipt> ExpectedSections;
    llvm::ArrayRef<COFFGeneratedEHSemanticBinding> ExpectedSemantics;
    if (ExpectedExceptionDirectory) {
      ExpectedOwners = ExpectedExceptionDirectory->GeneratedLanguageOwners;
      ExpectedSections = ExpectedExceptionDirectory->GeneratedSections;
      ExpectedSemantics = ExpectedExceptionDirectory->GeneratedEHSemantics;
    }
    if ((!ExpectedOwners.empty() || !ExpectedSections.empty() ||
         !ExpectedSemantics.empty()) &&
        !HasExpectedReplacement)
      return patchError(
          "generated receipts require a non-empty prepared exception table");
    for (size_t I = 0; I < ExpectedOwners.size(); ++I) {
      const COFFGeneratedLanguageOwnerReceipt &Receipt = ExpectedOwners[I];
      const bool ValidRole =
          Receipt.Role == COFFGeneratedLanguageOwnerRole::MappedRoot ||
          Receipt.Role == COFFGeneratedLanguageOwnerRole::CxxAuxiliary;
      const bool ValidModel =
          Receipt.Model == COFFGeneratedLanguageModel::SEH ||
          Receipt.Model == COFFGeneratedLanguageModel::CxxFH3 ||
          Receipt.Model == COFFGeneratedLanguageModel::CxxFH4;
      const bool ValidGSReceipt =
          Receipt.GSWrapped
              ? Receipt.Model == COFFGeneratedLanguageModel::CxxFH4 &&
                    Receipt.Role ==
                        COFFGeneratedLanguageOwnerRole::MappedRoot &&
                    isValidBoundedX64GSCookieHeader(Receipt.GSCookieHeader)
              : Receipt.GSCookieHeader == 0;
      if (Receipt.RuntimeWordCount != RecordSize / sizeof(uint32_t) ||
          Receipt.BeginRVA >= Receipt.EndRVA || Receipt.HandlerRVA == 0 ||
          Receipt.HandlerDataRVA == 0 ||
          Receipt.RuntimeFunctionRVA <
              ExceptionDirectory->RelativeVirtualAddress ||
          uint64_t(Receipt.RuntimeFunctionRVA) + RecordSize >
              uint64_t(ExceptionDirectory->RelativeVirtualAddress) +
                  ExceptionDirectory->Size ||
          (Receipt.RuntimeFunctionRVA -
           ExceptionDirectory->RelativeVirtualAddress) %
                  RecordSize !=
              0 ||
          !ValidModel || !ValidRole || !ValidGSReceipt ||
          (Receipt.Model == COFFGeneratedLanguageModel::SEH &&
           (Receipt.LanguageGroupRVA != 0 ||
            Receipt.Role != COFFGeneratedLanguageOwnerRole::MappedRoot)) ||
          (Receipt.Model == COFFGeneratedLanguageModel::CxxFH3 &&
           (!isFH3OutputArchitecture(TargetArch) ||
            Receipt.LanguageGroupRVA == 0)) ||
          (Receipt.Model == COFFGeneratedLanguageModel::CxxFH4 &&
           (TargetArch != Arch::X64 || Receipt.LanguageGroupRVA == 0 ||
            Receipt.Role != COFFGeneratedLanguageOwnerRole::MappedRoot)))
        return patchError("prepared generated language receipt is invalid");
      if (I != 0 && ExpectedOwners[I - 1].RuntimeFunctionRVA >=
                        Receipt.RuntimeFunctionRVA)
        return patchError(
            "prepared generated language receipts are not uniquely sorted");
    }

    auto encodeSEHCodeRVA = [&](uint32_t RVA) {
      return TargetArch == Arch::ARM && RVA != 0 ? RVA | uint32_t(1) : RVA;
    };
    for (size_t I = 0; I < ExpectedSemantics.size(); ++I) {
      const COFFGeneratedEHSemanticBinding &Binding = ExpectedSemantics[I];
      const bool IsSEH =
          Binding.Kind == COFFGeneratedEHSemanticKind::SEHScope &&
          Binding.Model == COFFGeneratedLanguageModel::SEH;
      const bool IsFH3 =
          Binding.Kind == COFFGeneratedEHSemanticKind::CxxCatch &&
          Binding.Model == COFFGeneratedLanguageModel::CxxFH3;
      const bool IsFH4 =
          Binding.Kind == COFFGeneratedEHSemanticKind::CxxCatch &&
          Binding.Model == COFFGeneratedLanguageModel::CxxFH4;
      const bool HasDigest = llvm::any_of(
          Binding.SourceDigest, [](uint64_t Word) { return Word != 0; });
      const size_t ExpectedByteCount = IsSEH   ? 4 * sizeof(uint32_t)
                                       : IsFH3 ? 5 * sizeof(uint32_t)
                                               : 0;
      if (!HasDigest || Binding.SourceFunctionVA == 0 ||
          Binding.SourceFunctionVA == InvalidVA ||
          ((!IsFH4 && Binding.RecordBytes.size() != ExpectedByteCount) ||
           (IsFH4 && Binding.RecordBytes.empty())) ||
          Binding.RecordRVA == 0 || Binding.GeneratedOwnerRVA == 0 ||
          Binding.ContainerRVA == 0 || Binding.HandlerRVA == 0 ||
          Binding.ContainerRVA > Binding.RecordRVA ||
          uint64_t(Binding.RecordRVA) + Binding.RecordBytes.size() >
              SizeOfImage ||
          (!IsSEH && !IsFH3 && !IsFH4))
        return patchError("prepared generated EH semantic binding is invalid");
      if (IsSEH) {
        const uint32_t Begin = readLE<uint32_t>(Binding.RecordBytes.data());
        const uint32_t End =
            readLE<uint32_t>(Binding.RecordBytes.data() + sizeof(uint32_t));
        const uint32_t Action =
            readLE<uint32_t>(Binding.RecordBytes.data() + 2 * sizeof(uint32_t));
        const uint32_t Handler =
            readLE<uint32_t>(Binding.RecordBytes.data() + 3 * sizeof(uint32_t));
        if (Binding.Clause != 0 || Binding.BeginRVA >= Binding.EndRVA ||
            Begin != encodeSEHCodeRVA(Binding.BeginRVA) ||
            End != encodeSEHCodeRVA(Binding.EndRVA) ||
            !((Action == encodeSEHCodeRVA(Binding.HandlerRVA) &&
               Handler == 0) ||
              Handler == encodeSEHCodeRVA(Binding.HandlerRVA)))
          return patchError(
              "prepared generated EH semantic binding is invalid");
      } else if (IsFH3) {
        if (!isFH3OutputArchitecture(TargetArch) || Binding.BeginRVA != 0 ||
            Binding.EndRVA != 0 ||
            readLE<uint32_t>(Binding.RecordBytes.data() +
                             3 * sizeof(uint32_t)) != Binding.HandlerRVA)
          return patchError(
              "prepared generated EH semantic binding is invalid");
      } else {
        auto Handler = parseBoundedFH4HandlerRow(Binding.RecordBytes);
        if (!Handler) {
          llvm::consumeError(Handler.takeError());
          return patchError(
              "prepared generated EH semantic binding is invalid");
        }
        if (TargetArch != Arch::X64 || Binding.BeginRVA != 0 ||
            Binding.EndRVA != 0 || Handler->HandlerRVA != Binding.HandlerRVA)
          return patchError(
              "prepared generated EH semantic binding is invalid");
      }
      if (I != 0) {
        const COFFGeneratedEHSemanticBinding &Previous =
            ExpectedSemantics[I - 1];
        if (Binding.RecordRVA <
            uint64_t(Previous.RecordRVA) + Previous.RecordBytes.size())
          return patchError(
              "prepared generated EH semantic bindings overlap or are not "
              "uniquely sorted");
      }

      size_t OwnerMatches = 0;
      for (const COFFGeneratedLanguageOwnerReceipt &Owner : ExpectedOwners) {
        const bool ModelMatches = Owner.Model == Binding.Model;
        OwnerMatches +=
            ModelMatches &&
            Owner.Role == COFFGeneratedLanguageOwnerRole::MappedRoot &&
            Owner.BeginRVA == Binding.GeneratedOwnerRVA;
      }
      if (OwnerMatches != 1)
        return patchError(
            "prepared generated EH semantic binding has no exact root owner");
    }

    struct ReceiptedFinalLanguageOwner {
      RuntimeEntry Entry;
      const COFFGeneratedLanguageOwnerReceipt *Receipt = nullptr;
    };
    std::vector<RuntimeEntry> Entries;
    std::vector<RuntimeEntry> GeneratedLanguageEntries;
    std::vector<ReceiptedFinalLanguageOwner> ReceiptedLanguageEntries;
    size_t Count = ExceptionDirectory->Size / RecordSize;
    Entries.reserve(Count);
    for (size_t I = 0; I < Count; ++I) {
      const uint8_t *Record = Binary.data() + *TableOffset + I * RecordSize;
      auto EntryOrErr = decodeRuntimeEntry(Record, TargetArch, ReadAtRVA);
      if (!EntryOrErr)
        return EntryOrErr.takeError();
      Entries.push_back(*EntryOrErr);

      const uint64_t RuntimeFunctionRVA64 =
          uint64_t(ExceptionDirectory->RelativeVirtualAddress) +
          uint64_t(I) * RecordSize;
      if (RuntimeFunctionRVA64 > std::numeric_limits<uint32_t>::max())
        return patchError(
            "final runtime-function record location exceeds PE limits");
      const uint32_t RuntimeFunctionRVA =
          static_cast<uint32_t>(RuntimeFunctionRVA64);
      const auto ExpectedIt = std::lower_bound(
          ExpectedOwners.begin(), ExpectedOwners.end(), RuntimeFunctionRVA,
          [](const COFFGeneratedLanguageOwnerReceipt &Receipt, uint32_t RVA) {
            return Receipt.RuntimeFunctionRVA < RVA;
          });
      const COFFGeneratedLanguageOwnerReceipt *ExpectedReceipt =
          ExpectedIt != ExpectedOwners.end() &&
                  ExpectedIt->RuntimeFunctionRVA == RuntimeFunctionRVA
              ? &*ExpectedIt
              : nullptr;
      const bool IsGeneratedLanguageOwner =
          EntryOrErr->HasLanguageHandler &&
          IsInGeneratedSection(EntryOrErr->Begin);
      if (ExpectedReceipt) {
        if (!IsGeneratedLanguageOwner ||
            EntryOrErr->WordCount != ExpectedReceipt->RuntimeWordCount ||
            EntryOrErr->Words != ExpectedReceipt->RuntimeWords ||
            EntryOrErr->Begin != ExpectedReceipt->BeginRVA ||
            EntryOrErr->End != ExpectedReceipt->EndRVA ||
            EntryOrErr->Unwind != ExpectedReceipt->UnwindRVA ||
            EntryOrErr->Handler != ExpectedReceipt->HandlerRVA ||
            EntryOrErr->HandlerData != ExpectedReceipt->HandlerDataRVA)
          return patchError(
              "final generated language owner does not match the prepared "
              "receipt");
        if (ExpectedReceipt->LanguageGroupRVA != 0) {
          const size_t HandlerDataSize =
              ExpectedReceipt->GSWrapped ? 2 * sizeof(uint32_t)
                                         : sizeof(uint32_t);
          const uint8_t *LanguageGroup =
              ReadAtRVA(EntryOrErr->HandlerData, HandlerDataSize);
          if (!LanguageGroup || readLE<uint32_t>(LanguageGroup) !=
                                    ExpectedReceipt->LanguageGroupRVA)
            return patchError(
                "final generated language owner does not match the prepared "
                "receipt");
          if (ExpectedReceipt->GSWrapped &&
              readLE<uint32_t>(LanguageGroup + sizeof(uint32_t)) !=
                  ExpectedReceipt->GSCookieHeader)
            return patchError(
                "final generated language owner GS header does not match the "
                "prepared receipt");
        }
        ReceiptedLanguageEntries.push_back({*EntryOrErr, ExpectedReceipt});
      } else if (IsGeneratedLanguageOwner && HasExpectedReplacement) {
        return patchError(
            "final generated language owner has no prepared receipt");
      }
      if (IsGeneratedLanguageOwner) {
        if (EntryOrErr->Begin >= EntryOrErr->End ||
            !IsInGeneratedSection(EntryOrErr->End - 1))
          return patchError(
              "final generated language owner leaves its section");
        if (TargetArch == Arch::ARM || TargetArch == Arch::AArch64) {
          if (!GeneratedSectionEndRVA ||
              !IsInGeneratedSection(EntryOrErr->Unwind) ||
              !IsInGeneratedSection(EntryOrErr->HandlerData))
            return patchError(
                "final generated ARM language metadata leaves its section");
          bool IsFH3Payload =
              ExpectedReceipt &&
              ExpectedReceipt->Model == COFFGeneratedLanguageModel::CxxFH3;
          if (!ExpectedReceipt && TargetArch == Arch::AArch64) {
            const uint8_t *HandlerData =
                ReadAtRVA(EntryOrErr->HandlerData, sizeof(uint32_t));
            if (HandlerData) {
              const uint32_t FuncInfoRVA = readLE<uint32_t>(HandlerData);
              const uint8_t *FuncInfo =
                  IsInGeneratedSection(FuncInfoRVA)
                      ? ReadAtRVA(FuncInfoRVA, sizeof(uint32_t))
                      : nullptr;
              IsFH3Payload =
                  FuncInfo && readLE<uint32_t>(FuncInfo) == FH3MagicNumber3;
            }
          }
          if (!IsFH3Payload) {
            if (llvm::Error Err = validateGeneratedARMCatchAllTable(
                    *EntryOrErr, TargetArch, *GeneratedSectionEndRVA,
                    ReadAtRVA))
              return std::move(Err);
          }
        }
        if (!ExpectedReceipt)
          GeneratedLanguageEntries.push_back(*EntryOrErr);
      }
      if (EntryOrErr->Begin >= EntryOrErr->End ||
          !IsExecutableRVA(EntryOrErr->Begin) ||
          !IsExecutableRVA(EntryOrErr->End - 1))
        return patchError(
            "final runtime-function range is not executable code");
      if (EntryOrErr->HasLanguageHandler &&
          !IsExecutableRVA(EntryOrErr->Handler))
        return patchError("final language handler is not executable");

      if (TargetArch != Arch::X64) {
        continue;
      }
      RuntimeEntry Current = *EntryOrErr;
      std::set<uint32_t> SeenUnwind;
      while (true) {
        if (Current.Begin >= Current.End || Current.End > SizeOfImage ||
            !IsExecutableRVA(Current.Begin) ||
            !IsExecutableRVA(Current.End - 1))
          return patchError("final chained x64 code range is invalid");
        if (!SeenUnwind.insert(Current.Unwind).second)
          return patchError("final x64 unwind chain is cyclic");
        if (Current.Unwind == 0 || Current.Unwind >= SizeOfImage ||
            (Current.Unwind & 3u) != 0)
          return patchError("final chained x64 unwind RVA is invalid");

        ExceptionFunction Decoded = coff_loader::decodeX64ExceptionFunction(
            ValidationImage, ImageBase, /*RuntimeFunctionRVA=*/0, Current.Begin,
            Current.End, Current.Unwind);
        if (Decoded.ParseStatus != ExceptionParseStatus::Complete) {
          std::string Message = "final x64 unwind record is not fully valid";
          if (!Decoded.Diagnostics.empty())
            Message += ": " + Decoded.Diagnostics.front();
          return patchError(Message);
        }
        if (Decoded.Kind == RuntimeFunctionKind::Chained) {
          if (!Decoded.ChainedPrimaryRange ||
              Decoded.ChainedUnwindInfoRVA == 0 ||
              Decoded.ChainedPrimaryRange->Begin < ImageBase ||
              Decoded.ChainedPrimaryRange->End < ImageBase ||
              Decoded.ChainedPrimaryRange->Begin - ImageBase >
                  std::numeric_limits<uint32_t>::max() ||
              Decoded.ChainedPrimaryRange->End - ImageBase >
                  std::numeric_limits<uint32_t>::max())
            return patchError("final chained x64 runtime record is invalid");
          Current.Begin = static_cast<uint32_t>(
              Decoded.ChainedPrimaryRange->Begin - ImageBase);
          Current.End = static_cast<uint32_t>(Decoded.ChainedPrimaryRange->End -
                                              ImageBase);
          Current.Unwind = Decoded.ChainedUnwindInfoRVA;
          continue;
        }
        break;
      }
    }
    if (!std::is_sorted(Entries.begin(), Entries.end(),
                        [](const RuntimeEntry &A, const RuntimeEntry &B) {
                          return A.Begin < B.Begin;
                        }))
      return patchError("final runtime-function entries are not RVA-sorted");
    if (llvm::Error Err =
            validateRuntimeEntries(Entries, SizeOfImage, TargetArch))
      return Err;
    if (ReceiptedLanguageEntries.size() != ExpectedOwners.size())
      return patchError(
          "final generated language owner receipt set is incomplete");
    if (HasExpectedReplacement && ExpectedSections.empty())
      return patchError(
          "prepared replacement exception table has no section receipts");

    for (size_t I = 0; I < ExpectedSections.size(); ++I) {
      const COFFGeneratedSectionReceipt &Receipt = ExpectedSections[I];
      if (Receipt.Size == 0 ||
          uint64_t(Receipt.RVA) + Receipt.Size > SizeOfImage ||
          !IsInGeneratedSection(Receipt.RVA) ||
          !IsInGeneratedSection(Receipt.RVA + Receipt.Size - 1))
        return patchError("prepared generated section receipt is invalid");
      if (I != 0) {
        const COFFGeneratedSectionReceipt &Previous = ExpectedSections[I - 1];
        if (Receipt.RVA < uint64_t(Previous.RVA) + Previous.Size)
          return patchError(
              "prepared generated section receipts are not uniquely sorted");
      }
      auto SectionOffset =
          rvaToFileOffset(PE, Binary, Receipt.RVA, Receipt.Size);
      if (!SectionOffset)
        return patchError("final generated section receipt is not file-backed");
      const std::array<uint8_t, 32> Digest =
          llvm::SHA256::hash(Binary.slice(*SectionOffset, Receipt.Size));
      if (Digest != Receipt.SHA256)
        return patchError(
            "final generated section does not match the prepared receipt");
    }

    auto IsCoveredByExpectedSection = [&](uint32_t RVA, size_t Size) {
      return std::any_of(ExpectedSections.begin(), ExpectedSections.end(),
                         [&](const COFFGeneratedSectionReceipt &Section) {
                           return RVA >= Section.RVA &&
                                  uint64_t(RVA) + Size <=
                                      uint64_t(Section.RVA) + Section.Size;
                         });
    };
    if (HasExpectedReplacement &&
        !IsCoveredByExpectedSection(ExpectedExceptionDirectory->RVA,
                                    ExpectedExceptionDirectory->Size))
      return patchError(
          "prepared replacement exception table is not section-authenticated");
    for (const COFFGeneratedLanguageOwnerReceipt &Receipt : ExpectedOwners) {
      if (!IsCoveredByExpectedSection(Receipt.BeginRVA,
                                      Receipt.EndRVA - Receipt.BeginRVA) ||
          !IsCoveredByExpectedSection(Receipt.UnwindRVA, sizeof(uint32_t)) ||
          !IsCoveredByExpectedSection(
              Receipt.HandlerDataRVA,
              Receipt.GSWrapped ? 2 * sizeof(uint32_t) : sizeof(uint32_t)) ||
          (Receipt.LanguageGroupRVA != 0 &&
           !IsCoveredByExpectedSection(
               Receipt.LanguageGroupRVA,
               Receipt.Model == COFFGeneratedLanguageModel::CxxFH3
                   ? FH3FuncInfoSize
                   : 1)))
        return patchError(
            "prepared generated language receipt is not section-authenticated");
    }

    using SemanticRowIdentity =
        std::tuple<COFFGeneratedEHSemanticKind, uint32_t, uint32_t, uint32_t>;
    std::set<SemanticRowIdentity> BoundSemanticRows;
    for (const COFFGeneratedEHSemanticBinding &Binding : ExpectedSemantics) {
      const size_t RecordSize = Binding.RecordBytes.size();
      const size_t ContainerSize =
          Binding.Model == COFFGeneratedLanguageModel::CxxFH3
              ? FH3TryBlockSize
              : sizeof(uint32_t);
      if (!IsCoveredByExpectedSection(Binding.RecordRVA, RecordSize) ||
          !IsCoveredByExpectedSection(Binding.GeneratedOwnerRVA, 1) ||
          !IsCoveredByExpectedSection(Binding.ContainerRVA, ContainerSize) ||
          !IsCoveredByExpectedSection(Binding.HandlerRVA, 1) ||
          (Binding.Kind == COFFGeneratedEHSemanticKind::SEHScope &&
           (!IsCoveredByExpectedSection(Binding.BeginRVA,
                                        Binding.EndRVA - Binding.BeginRVA))))
        return patchError("prepared generated EH semantic binding is not "
                          "section-authenticated");
      const uint8_t *Record = ReadAtRVA(Binding.RecordRVA, RecordSize);
      if (!Record)
        return patchError("final generated EH semantic row is not file-backed");
      if (!std::equal(Binding.RecordBytes.begin(), Binding.RecordBytes.end(),
                      Record))
        return patchError(
            "final generated EH semantic row does not match its prepared "
            "binding");
      if (!BoundSemanticRows
               .emplace(Binding.Kind, Binding.GeneratedOwnerRVA,
                        Binding.ContainerRVA, Binding.RecordRVA)
               .second)
        return patchError(
            "prepared generated EH semantic row is bound more than once");
    }

    std::set<SemanticRowIdentity> PhysicalSemanticRows;
    auto AddPhysicalRows = [&](COFFGeneratedEHSemanticKind Kind,
                               uint32_t RootOwnerRVA, uint32_t ContainerRVA,
                               uint32_t FirstRVA, uint32_t Count,
                               uint32_t Stride) -> llvm::Error {
      if (Stride == 0 || Count > SizeOfImage / Stride ||
          uint64_t(FirstRVA) + uint64_t(Count) * Stride > SizeOfImage)
        return patchError(
            "final generated EH semantic table has an invalid extent");
      for (uint32_t I = 0; I < Count; ++I) {
        const uint64_t RowRVA = uint64_t(FirstRVA) + uint64_t(I) * Stride;
        if (RowRVA > std::numeric_limits<uint32_t>::max() ||
            !ReadAtRVA(static_cast<uint32_t>(RowRVA), Stride) ||
            !PhysicalSemanticRows
                 .emplace(Kind, RootOwnerRVA, ContainerRVA,
                          static_cast<uint32_t>(RowRVA))
                 .second)
          return patchError(
              "final generated EH semantic table has duplicate or invalid "
              "rows");
      }
      return llvm::Error::success();
    };
    for (const COFFGeneratedLanguageOwnerReceipt &Owner : ExpectedOwners) {
      if (Owner.Role != COFFGeneratedLanguageOwnerRole::MappedRoot)
        continue;
      if (Owner.Model == COFFGeneratedLanguageModel::SEH) {
        const uint8_t *Header =
            ReadAtRVA(Owner.HandlerDataRVA, sizeof(uint32_t));
        if (!Header ||
            Owner.HandlerDataRVA >
                std::numeric_limits<uint32_t>::max() - sizeof(uint32_t))
          return patchError("final generated SEH scope table is truncated");
        const uint32_t ScopeCount = readLE<uint32_t>(Header);
        if (ScopeCount > SizeOfImage / (4 * sizeof(uint32_t)))
          return patchError(
              "final generated SEH scope table has an invalid extent");
        const size_t ScopeTableSize =
            sizeof(uint32_t) + size_t(ScopeCount) * 4 * sizeof(uint32_t);
        if (!IsCoveredByExpectedSection(Owner.HandlerDataRVA, ScopeTableSize))
          return patchError(
              "final generated SEH scope table is not section-authenticated");
        if (llvm::Error Err = AddPhysicalRows(
                COFFGeneratedEHSemanticKind::SEHScope, Owner.BeginRVA,
                Owner.HandlerDataRVA + sizeof(uint32_t),
                Owner.HandlerDataRVA + sizeof(uint32_t), ScopeCount,
                4 * sizeof(uint32_t)))
          return Err;
        continue;
      }

      if (Owner.Model == COFFGeneratedLanguageModel::CxxFH4) {
        auto Layout =
            parseGeneratedFH4Layout(Owner.LanguageGroupRVA, ReadAtRVA);
        if (!Layout)
          return Layout.takeError();
        if (llvm::Error Err = validateBoundedGeneratedFH4Layout(*Layout))
          return std::move(Err);
        auto RequireCovered =
            [&](const coff_fh4::ByteRange &Range,
                const llvm::Twine &Description) -> llvm::Error {
          if (!IsCoveredByExpectedSection(Range.BeginRVA, Range.size()))
            return patchError("final generated " + Description +
                              " is not section-authenticated");
          return llvm::Error::success();
        };
        if (llvm::Error Err =
                RequireCovered(Layout->HeaderRange, "C++ EH4 FuncInfo"))
          return std::move(Err);
        if (llvm::Error Err =
                RequireCovered(Layout->Unwind->Range, "C++ EH4 unwind map"))
          return std::move(Err);
        if (llvm::Error Err =
                RequireCovered(Layout->Try->Range, "C++ EH4 try map"))
          return std::move(Err);
        if (llvm::Error Err =
                RequireCovered(Layout->States->Range, "C++ EH4 IP map"))
          return std::move(Err);
        for (const coff_fh4::TryEntry &Try : Layout->Try->Entries) {
          if (llvm::Error Err =
                  RequireCovered(Try.Handlers.Range, "C++ EH4 handler map"))
            return std::move(Err);
          for (const coff_fh4::HandlerEntry &Handler : Try.Handlers.Entries) {
            if (!PhysicalSemanticRows
                     .emplace(COFFGeneratedEHSemanticKind::CxxCatch,
                              Owner.BeginRVA, Try.Range.BeginRVA,
                              Handler.Range.BeginRVA)
                     .second)
              return patchError(
                  "final generated C++ EH4 handler map has duplicate rows");
          }
        }
        continue;
      }

      if (Owner.Model != COFFGeneratedLanguageModel::CxxFH3)
        return patchError(
            "final generated C++ owner has an unknown language model");

      const uint8_t *FuncInfo =
          ReadAtRVA(Owner.LanguageGroupRVA, FH3FuncInfoSize);
      if (!FuncInfo || !IsCoveredByExpectedSection(Owner.LanguageGroupRVA,
                                                   FH3FuncInfoSize))
        return patchError("final generated C++ FuncInfo is truncated");
      const int32_t MaxState = readLE<int32_t>(FuncInfo + sizeof(uint32_t));
      const uint32_t UnwindMapRVA =
          readLE<uint32_t>(FuncInfo + 2 * sizeof(uint32_t));
      const uint32_t NumTryBlocks =
          readLE<uint32_t>(FuncInfo + 3 * sizeof(uint32_t));
      const uint32_t TryMapRVA =
          readLE<uint32_t>(FuncInfo + 4 * sizeof(uint32_t));
      const uint32_t NumIPStates =
          readLE<uint32_t>(FuncInfo + 5 * sizeof(uint32_t));
      const uint32_t IPMapRVA =
          readLE<uint32_t>(FuncInfo + 6 * sizeof(uint32_t));
      if (MaxState < 0 ||
          static_cast<uint32_t>(MaxState) >
              SizeOfImage / FH3StateMapEntrySize ||
          NumTryBlocks == 0 ||
          NumTryBlocks > SizeOfImage / FH3TryBlockSize || NumIPStates == 0 ||
          NumIPStates > SizeOfImage / FH3StateMapEntrySize ||
          uint64_t(TryMapRVA) + uint64_t(NumTryBlocks) * FH3TryBlockSize >
              SizeOfImage)
        return patchError("final generated C++ try map has an invalid extent");
      auto IsStateMapCovered = [&](uint32_t RVA, size_t Count, size_t Stride) {
        return IsCoveredByExpectedSection(RVA, Count * Stride);
      };
      if ((MaxState != 0 &&
           !IsStateMapCovered(UnwindMapRVA, static_cast<uint32_t>(MaxState),
                              FH3StateMapEntrySize)) ||
          !IsStateMapCovered(TryMapRVA, NumTryBlocks, FH3TryBlockSize) ||
          !IsStateMapCovered(IPMapRVA, NumIPStates, FH3StateMapEntrySize))
        return patchError(
            "final generated C++ state maps are not section-authenticated");
      for (uint32_t TryIndex = 0; TryIndex < NumTryBlocks; ++TryIndex) {
        const uint64_t TryRVA =
            uint64_t(TryMapRVA) + uint64_t(TryIndex) * FH3TryBlockSize;
        const uint8_t *Try =
            ReadAtRVA(static_cast<uint32_t>(TryRVA), FH3TryBlockSize);
        if (!Try)
          return patchError("final generated C++ try map is truncated");
        const uint32_t NumCatches =
            readLE<uint32_t>(Try + 3 * sizeof(uint32_t));
        const uint32_t HandlerMapRVA =
            readLE<uint32_t>(Try + 4 * sizeof(uint32_t));
        if (NumCatches == 0 ||
            NumCatches > SizeOfImage / FH3HandlerTypeSize ||
            !IsStateMapCovered(HandlerMapRVA, NumCatches,
                               FH3HandlerTypeSize))
          return patchError(
              "final generated C++ handler map is not section-authenticated");
        if (llvm::Error Err = AddPhysicalRows(
                COFFGeneratedEHSemanticKind::CxxCatch, Owner.BeginRVA,
                static_cast<uint32_t>(TryRVA), HandlerMapRVA, NumCatches,
                FH3HandlerTypeSize))
          return Err;
      }
    }
    if (PhysicalSemanticRows != BoundSemanticRows)
      return patchError(
          "final generated EH semantic bindings do not close the physical "
          "language tables");

    const uint32_t PointerSize = PE.Is64 ? 8 : 4;
    for (auto I = COFF->import_directory_begin(),
              E = COFF->import_directory_end();
         I != E; ++I) {
      llvm::StringRef DLLName;
      if (llvm::Error Err = I->getName(DLLName)) {
        llvm::consumeError(std::move(Err));
        continue;
      }
      uint32_t IATRVA = 0;
      if (llvm::Error Err = I->getImportAddressTableRVA(IATRVA)) {
        llvm::consumeError(std::move(Err));
        continue;
      }
      uint32_t Index = 0;
      for (auto SI = I->imported_symbol_begin(), SE = I->imported_symbol_end();
           SI != SE; ++SI, ++Index) {
        const uint64_t SlotRVA =
            uint64_t(IATRVA) + uint64_t(Index) * PointerSize;
        if (SlotRVA > InvalidVA - ImageBase)
          break;
        coff_loader::addImportedSymbol(SI, DLLName, ImageBase + SlotRVA,
                                       ValidationImage);
      }
    }
    for (auto I = COFF->export_directory_begin(),
              E = COFF->export_directory_end();
         I != E; ++I) {
      llvm::StringRef Name;
      uint32_t RVA = 0;
      uint32_t Ordinal = 0;
      if (llvm::Error Err = I->getSymbolName(Name)) {
        llvm::consumeError(std::move(Err));
        continue;
      }
      if (llvm::Error Err = I->getExportRVA(RVA)) {
        llvm::consumeError(std::move(Err));
        continue;
      }
      if (llvm::Error Err = I->getOrdinal(Ordinal)) {
        llvm::consumeError(std::move(Err));
        continue;
      }
      if (RVA > InvalidVA - ImageBase)
        continue;
      va_t Address = ImageBase + RVA;
      const Segment *Target = ValidationImage.getSegmentFor(Address);
      if (Target && Target->isExecutable())
        Address =
            normalizeCodeAddress(Address, TargetArch, ValidationImage.Mode);
      Export Exp;
      Exp.Name = Name.str();
      Exp.Ordinal = Ordinal;
      Exp.Addr = Address;
      ValidationImage.Exports.push_back(std::move(Exp));
    }
    coff_loader::parseDelayImports(*COFF, ValidationImage);
    coff_loader::parseExceptions(*COFF, ValidationImage, ImageBase);
    coff_loader::parseSymbolTable(*COFF, ValidationImage, ImageBase);
    coff_loader::resolveExceptionHandlers(ValidationImage);
    for (const ExceptionFunction &EH :
         ValidationImage.ExceptionMetadata.Functions) {
      if (EH.ParseStatus == ExceptionParseStatus::Malformed) {
        std::string Message =
            "final language exception metadata is malformed at 0x" +
            llvm::utohexstr(EH.CodeRange.Begin);
        if (!EH.Diagnostics.empty())
          Message += ": " + EH.Diagnostics.front();
        if (EH.Cxx && !EH.Cxx->IPMap.empty())
          Message += " (range 0x" + llvm::utohexstr(EH.CodeRange.Begin) +
                     "..0x" + llvm::utohexstr(EH.CodeRange.End) +
                     ", first IP 0x" +
                     llvm::utohexstr(EH.Cxx->IPMap.front().IP) + ")";
        return patchError(Message);
      }
      if ((EH.Personality == ExceptionPersonality::CSpecificHandler ||
           EH.Personality == ExceptionPersonality::CxxFrameHandler3 ||
           EH.Personality == ExceptionPersonality::CxxFrameHandler4 ||
           EH.Personality == ExceptionPersonality::GSHandlerCheckEH4) &&
          EH.ParseStatus != ExceptionParseStatus::Complete)
        return patchError(
            "final supported language exception metadata is incomplete");
    }
    struct ReconstructedLanguageOwner {
      const COFFGeneratedLanguageOwnerReceipt *Receipt = nullptr;
      const ExceptionFunction *EH = nullptr;
    };
    std::vector<ReconstructedLanguageOwner> ReconstructedOwners;
    ReconstructedOwners.reserve(ReceiptedLanguageEntries.size());
    for (const ReceiptedFinalLanguageOwner &FinalOwner :
         ReceiptedLanguageEntries) {
      const COFFGeneratedLanguageOwnerReceipt &Receipt = *FinalOwner.Receipt;
      const va_t BeginVA = ImageBase + Receipt.BeginRVA;
      const va_t EndVA = ImageBase + Receipt.EndRVA;
      const va_t PersonalityVA = ImageBase + Receipt.HandlerRVA;
      const va_t HandlerDataVA = ImageBase + Receipt.HandlerDataRVA;
      const ExceptionFunction *GeneratedEH = nullptr;
      for (const ExceptionFunction &EH :
           ValidationImage.ExceptionMetadata.Functions) {
        if (EH.RuntimeFunctionRVA != Receipt.RuntimeFunctionRVA ||
            EH.CodeRange.Begin != BeginVA || EH.CodeRange.End != EndVA ||
            EH.UnwindInfoRVA != Receipt.UnwindRVA ||
            normalizeCodeAddress(EH.PersonalityVA, TargetArch,
                                 ValidationImage.Mode) != PersonalityVA ||
            EH.HandlerDataVA != HandlerDataVA)
          continue;
        if (GeneratedEH)
          return patchError(
              "final generated language owner was reconstructed ambiguously");
        GeneratedEH = &EH;
      }
      if (!GeneratedEH)
        return patchError(
            "final generated language owner was not reconstructed");
      const bool MatchesFH4Receipt =
          Receipt.Model == COFFGeneratedLanguageModel::CxxFH4 &&
          ((Receipt.GSWrapped &&
            GeneratedEH->Personality ==
                ExceptionPersonality::GSHandlerCheckEH4 &&
            hasExactReconstructedGSCookie(*GeneratedEH,
                                          Receipt.GSCookieHeader)) ||
           (!Receipt.GSWrapped && Receipt.GSCookieHeader == 0 &&
            GeneratedEH->Personality ==
                ExceptionPersonality::CxxFrameHandler4 &&
            !GeneratedEH->GSCookie));
      if (GeneratedEH->Kind != RuntimeFunctionKind::Primary ||
          GeneratedEH->ParseStatus != ExceptionParseStatus::Complete ||
          (Receipt.Model == COFFGeneratedLanguageModel::SEH &&
           GeneratedEH->Personality !=
               ExceptionPersonality::CSpecificHandler) ||
          (Receipt.Model == COFFGeneratedLanguageModel::CxxFH3 &&
           GeneratedEH->Personality !=
               ExceptionPersonality::CxxFrameHandler3) ||
          (Receipt.Model == COFFGeneratedLanguageModel::CxxFH4 &&
           !MatchesFH4Receipt))
        return patchError(
            "final generated language owner model does not match its receipt");
      ReconstructedOwners.push_back({&Receipt, GeneratedEH});
    }

    std::map<uint32_t, std::vector<const ReconstructedLanguageOwner *>>
        CxxGroups;
    for (const ReconstructedLanguageOwner &Owner : ReconstructedOwners) {
      const COFFGeneratedLanguageOwnerReceipt &Receipt = *Owner.Receipt;
      if (Receipt.Model == COFFGeneratedLanguageModel::CxxFH3) {
        CxxGroups[Receipt.LanguageGroupRVA].push_back(&Owner);
        continue;
      }
      if (Receipt.Model == COFFGeneratedLanguageModel::CxxFH4) {
        if (Receipt.Role != COFFGeneratedLanguageOwnerRole::MappedRoot ||
            !Owner.EH->Cxx ||
            Owner.EH->Cxx->NativeEncoding != CxxExceptionInfo::Encoding::FH4 ||
            Owner.EH->Cxx->NativeFuncInfoVA !=
                ImageBase + Receipt.LanguageGroupRVA)
          return patchError(
              "final generated C++ EH4 owner is structurally invalid");

        const COFFGeneratedEHSemanticBinding *CatchBinding = nullptr;
        for (const COFFGeneratedEHSemanticBinding &Binding :
             ExpectedSemantics) {
          if (Binding.Model != COFFGeneratedLanguageModel::CxxFH4 ||
              Binding.Kind != COFFGeneratedEHSemanticKind::CxxCatch ||
              Binding.GeneratedOwnerRVA != Receipt.BeginRVA)
            continue;
          if (CatchBinding)
            return patchError(
                "final generated C++ EH4 owner has ambiguous catch bindings");
          CatchBinding = &Binding;
        }
        if (!CatchBinding || Owner.EH->Cxx->TryBlocks.size() != 1 ||
            Owner.EH->Cxx->TryBlocks.front().Handlers.size() != 1 ||
            Owner.EH->Cxx->TryBlocks.front().Handlers.front().HandlerVA !=
                ImageBase + CatchBinding->HandlerRVA)
          return patchError(
              "final generated C++ EH4 owner lacks its exact catch binding");

        const ExceptionFunction *CatchOwner = nullptr;
        for (const ExceptionFunction &Candidate :
             ValidationImage.ExceptionMetadata.Functions) {
          if (Candidate.Kind != RuntimeFunctionKind::Primary ||
              Candidate.CodeRange.Begin != ImageBase + CatchBinding->HandlerRVA)
            continue;
          if (CatchOwner)
            return patchError(
                "final generated C++ EH4 catch has ambiguous runtime owners");
          CatchOwner = &Candidate;
        }
        if (!CatchOwner ||
            CatchOwner->ParseStatus != ExceptionParseStatus::Complete ||
            CatchOwner->Personality != ExceptionPersonality::None ||
            !IsInGeneratedSection(static_cast<uint32_t>(
                CatchOwner->CodeRange.Begin - ImageBase)) ||
            Owner.EH->CodeRange.overlaps(CatchOwner->CodeRange) ||
            CatchOwner->CodeRange.Begin < Owner.EH->CodeRange.End)
          return patchError(
              "final generated C++ EH4 catch has no exact unwind owner");

        ExceptionFunction Normalized = *Owner.EH;
        Normalized.CodeRange.Begin =
            std::min(Normalized.CodeRange.Begin, CatchOwner->CodeRange.Begin);
        Normalized.CodeRange.End =
            std::max(Normalized.CodeRange.End, CatchOwner->CodeRange.End);
        const WindowsEHNativeSourceClassification Classification =
            classifyWindowsEHNativeSource(
                Normalized, Arch::X64, BinaryFormat::COFF,
                WindowsEHNativeCapability::OutputPatch);
        if (!Classification.canPatchOutput())
          return patchError(
              "final generated x64 C++ EH4 metadata failed output checks: " +
              llvm::Twine(
                  getWindowsEHNativeSourceReasonName(Classification.Reason)));
        continue;
      }
      if (Receipt.Role != COFFGeneratedLanguageOwnerRole::MappedRoot)
        return patchError(
            "final generated SEH owner has an invalid receipt role");
      const WindowsEHNativeSourceClassification Classification =
          classifyWindowsEHNativeSource(*Owner.EH, TargetArch,
                                        BinaryFormat::COFF,
                                        WindowsEHNativeCapability::OutputPatch);
      if (!Classification.canPatchOutput()) {
        const llvm::StringRef Architecture =
            TargetArch == Arch::AArch64 ? "ARM64" : "x64";
        return patchError("final generated " + Architecture +
                          " language metadata failed output checks: " +
                          llvm::Twine(getWindowsEHNativeSourceReasonName(
                              Classification.Reason)));
      }
    }

    for (const auto &[GroupRVA, Members] : CxxGroups) {
      if (GroupRVA == 0 || Members.empty())
        return patchError("final generated C++ language group is empty");
      const ReconstructedLanguageOwner *Root = nullptr;
      uint32_t HandlerRVA = 0;
      std::vector<const ExceptionFunction *> GroupFunctions;
      std::vector<ExceptionAddressRange> GroupRanges;
      GroupFunctions.reserve(Members.size());
      GroupRanges.reserve(Members.size());
      for (const ReconstructedLanguageOwner *Member : Members) {
        const COFFGeneratedLanguageOwnerReceipt &Receipt = *Member->Receipt;
        const ExceptionFunction &EH = *Member->EH;
        if (!EH.Cxx ||
            EH.Cxx->NativeEncoding != CxxExceptionInfo::Encoding::FH3 ||
            EH.Cxx->NativeFuncInfoVA != ImageBase + GroupRVA ||
            !EH.Cxx->hasValidStateGraph() ||
            EH.Cxx->IsSeparated != (Members.size() > 1))
          return patchError(
              "final generated C++ language group is structurally invalid");
        if (HandlerRVA == 0)
          HandlerRVA = Receipt.HandlerRVA;
        else if (HandlerRVA != Receipt.HandlerRVA)
          return patchError(
              "final generated C++ language group changes personality");
        if (Receipt.Role == COFFGeneratedLanguageOwnerRole::MappedRoot) {
          if (Root)
            return patchError(
                "final generated C++ language group has multiple mapped "
                "roots");
          Root = Member;
        } else if (Receipt.Role !=
                   COFFGeneratedLanguageOwnerRole::CxxAuxiliary) {
          return patchError(
              "final generated C++ language group has an invalid role");
        }
        GroupFunctions.push_back(&EH);
        GroupRanges.push_back(EH.CodeRange);
      }
      if (!Root || Root->EH->Cxx->IsCatchFunclet)
        return patchError(
            "final generated C++ language group has no canonical root");

      std::sort(
          GroupRanges.begin(), GroupRanges.end(),
          [](const ExceptionAddressRange &A, const ExceptionAddressRange &B) {
            return std::tie(A.Begin, A.End) < std::tie(B.Begin, B.End);
          });
      for (size_t I = 1; I < GroupRanges.size(); ++I)
        if (GroupRanges[I - 1].overlaps(GroupRanges[I]))
          return patchError(
              "final generated C++ language group ranges overlap");

      size_t LoaderGroupMembers = 0;
      for (const ExceptionFunction &EH :
           ValidationImage.ExceptionMetadata.Functions) {
        if (!EH.Cxx || EH.Cxx->NativeFuncInfoVA != ImageBase + GroupRVA ||
            EH.CodeRange.Begin < ImageBase ||
            EH.CodeRange.Begin - ImageBase >
                std::numeric_limits<uint32_t>::max() ||
            !IsInGeneratedSection(
                static_cast<uint32_t>(EH.CodeRange.Begin - ImageBase)))
          continue;
        ++LoaderGroupMembers;
      }
      if (LoaderGroupMembers != Members.size())
        return patchError(
            "final generated C++ language group does not match its receipt "
            "membership");

      size_t NonCatchMembers = 0;
      for (const ExceptionFunction *EH : GroupFunctions)
        NonCatchMembers += !EH->Cxx->IsCatchFunclet;
      if (NonCatchMembers != 1)
        return patchError(
            "final generated C++ language group has no unique parent");

      auto IsInGroupRange = [&](va_t Address, bool AllowEnd) {
        return std::any_of(GroupRanges.begin(), GroupRanges.end(),
                           [&](const ExceptionAddressRange &Range) {
                             return Range.contains(Address) ||
                                    (AllowEnd && Address == Range.End);
                           });
      };
      const CxxExceptionInfo &Cxx = *Root->EH->Cxx;
      std::set<va_t> ReferencedCatchOwners;
      for (const CxxIPState &IP : Cxx.IPMap)
        if (!IsInGroupRange(IP.IP, /*AllowEnd=*/true))
          return patchError(
              "final generated C++ IP map leaves its receipted group");
      for (const CxxTryBlock &Try : Cxx.TryBlocks) {
        for (const CxxCatchHandler &Catch : Try.Handlers) {
          size_t CatchOwners = 0;
          for (const ExceptionFunction *EH : GroupFunctions)
            CatchOwners += EH->CodeRange.Begin == Catch.HandlerVA &&
                           EH->Cxx->IsCatchFunclet;
          if (CatchOwners != 1 ||
              !IsInGroupRange(Catch.HandlerVA, /*AllowEnd=*/false) ||
              Catch.ParentFrameOffset < 0 ||
              (Catch.ParentFrameOffset & (PointerSize - 1)) != 0)
            return patchError(
                "final generated C++ catch does not name an exact group "
                "funclet");
          ReferencedCatchOwners.insert(Catch.HandlerVA);
        }
      }
      for (const ReconstructedLanguageOwner *Member : Members) {
        if (Member->Receipt->Role !=
            COFFGeneratedLanguageOwnerRole::CxxAuxiliary)
          continue;
        if (!Member->EH->Cxx->IsCatchFunclet ||
            !ReferencedCatchOwners.count(Member->EH->CodeRange.Begin))
          return patchError(
              "final generated C++ auxiliary is not referenced by a catch");
      }

      ExceptionFunction NormalizedRoot = *Root->EH;
      NormalizedRoot.CodeRange = {GroupRanges.front().Begin,
                                  GroupRanges.back().End};
      NormalizedRoot.Cxx->IsCatchFunclet = false;
      NormalizedRoot.Cxx->IsSeparated = false;
      for (CxxTryBlock &Try : NormalizedRoot.Cxx->TryBlocks)
        for (CxxCatchHandler &Catch : Try.Handlers)
          Catch.ParentFrameOffset = 0;
      const WindowsEHNativeSourceClassification Classification =
          classifyWindowsEHNativeSource(NormalizedRoot, TargetArch,
                                        BinaryFormat::COFF,
                                        WindowsEHNativeCapability::OutputPatch);
      if (!Classification.canPatchOutput())
        return patchError("final generated C++ language group failed "
                          "target-aware output checks: " +
                          llvm::Twine(getWindowsEHNativeSourceReasonName(
                              Classification.Reason)));
    }

    for (const RuntimeEntry &Entry : GeneratedLanguageEntries) {
      const va_t BeginVA = ImageBase + Entry.Begin;
      const va_t EndVA = ImageBase + Entry.End;
      const auto GeneratedEH = std::find_if(
          ValidationImage.ExceptionMetadata.Functions.begin(),
          ValidationImage.ExceptionMetadata.Functions.end(),
          [&](const ExceptionFunction &EH) {
            return EH.CodeRange.Begin == BeginVA && EH.CodeRange.End == EndVA &&
                   EH.UnwindInfoRVA == Entry.Unwind &&
                   normalizeCodeAddress(EH.PersonalityVA, TargetArch,
                                        ValidationImage.Mode) ==
                       ImageBase + Entry.Handler &&
                   EH.HandlerDataVA == ImageBase + Entry.HandlerData;
          });
      if (GeneratedEH == ValidationImage.ExceptionMetadata.Functions.end())
        return patchError(
            "final generated language owner was not reconstructed");
      if (GeneratedEH->Personality == ExceptionPersonality::CxxFrameHandler3 ||
          GeneratedEH->Personality == ExceptionPersonality::CxxFrameHandler4 ||
          GeneratedEH->Personality == ExceptionPersonality::GSHandlerCheckEH4)
        return patchError(
            "final generated C++ language group lacks a prepared receipt");
      const WindowsEHNativeSourceClassification Classification =
          classifyWindowsEHNativeSource(*GeneratedEH, TargetArch,
                                        BinaryFormat::COFF,
                                        WindowsEHNativeCapability::OutputPatch);
      if (!Classification.canPatchOutput()) {
        const llvm::StringRef Architecture = TargetArch == Arch::ARM ? "ARM32"
                                             : TargetArch == Arch::AArch64
                                                 ? "ARM64"
                                                 : "x64";
        return patchError("final generated " + Architecture +
                          " language metadata failed output checks: " +
                          llvm::Twine(getWindowsEHNativeSourceReasonName(
                              Classification.Reason)));
      }
    }
  }

  const llvm::object::data_directory *LoadConfigDirectory =
      getPEDataDirectory(PE, llvm::COFF::LOAD_CONFIG_TABLE);
  if (!LoadConfigDirectory || LoadConfigDirectory->Size == 0)
    return llvm::Error::success();
  if (LoadConfigDirectory->RelativeVirtualAddress == 0)
    return patchError("final load-config directory has a zero RVA");
  if (LoadConfigDirectory->RelativeVirtualAddress >= SizeOfImage ||
      LoadConfigDirectory->Size >
          SizeOfImage - LoadConfigDirectory->RelativeVirtualAddress)
    return patchError("final load configuration leaves the image extent");
  auto LoadOffset =
      rvaToFileOffset(PE, Binary, LoadConfigDirectory->RelativeVirtualAddress,
                      LoadConfigDirectory->Size);
  if (!LoadOffset)
    return patchError("final load configuration is not file-backed");
  if (LoadConfigDirectory->Size < sizeof(uint32_t))
    return patchError("final load configuration has no size field");
  uint32_t DeclaredLoadSize = readLE<uint32_t>(Binary.data() + *LoadOffset);
  if (DeclaredLoadSize < sizeof(uint32_t))
    return patchError("final load configuration declares an invalid size");
  uint32_t EffectiveLoadSize =
      std::min<uint32_t>(LoadConfigDirectory->Size, DeclaredLoadSize);

  uint32_t GuardFlags = 0;
  uint64_t CFTableVA = 0, CFCount = 0, EHTableVA = 0, EHCount = 0;
  bool HasCFTableField = false, HasCFCountField = false;
  bool HasEHTableField = false, HasEHCountField = false;
  auto ReadField = [&](size_t Offset, size_t Width, uint64_t &Value) -> bool {
    if (!rangeInBounds(Offset, Width, EffectiveLoadSize) ||
        !rangeInBounds(*LoadOffset + Offset, Width, Binary.size()))
      return false;
    Value = Width == 8 ? readLE<uint64_t>(Binary.data() + *LoadOffset + Offset)
                       : readLE<uint32_t>(Binary.data() + *LoadOffset + Offset);
    return true;
  };
  using llvm::object::coff_load_configuration32;
  using llvm::object::coff_load_configuration64;
  if (PE.Is64) {
    uint64_t Flags = 0;
    if (ReadField(offsetof(coff_load_configuration64, GuardFlags), 4, Flags))
      GuardFlags = static_cast<uint32_t>(Flags);
    HasCFTableField =
        ReadField(offsetof(coff_load_configuration64, GuardCFFunctionTable), 8,
                  CFTableVA);
    HasCFCountField = ReadField(
        offsetof(coff_load_configuration64, GuardCFFunctionCount), 8, CFCount);
    HasEHTableField =
        ReadField(offsetof(coff_load_configuration64, GuardEHContinuationTable),
                  8, EHTableVA);
    HasEHCountField =
        ReadField(offsetof(coff_load_configuration64, GuardEHContinuationCount),
                  8, EHCount);
  } else {
    uint64_t Flags = 0;
    if (ReadField(offsetof(coff_load_configuration32, GuardFlags), 4, Flags))
      GuardFlags = static_cast<uint32_t>(Flags);
    HasCFTableField =
        ReadField(offsetof(coff_load_configuration32, GuardCFFunctionTable), 4,
                  CFTableVA);
    HasCFCountField = ReadField(
        offsetof(coff_load_configuration32, GuardCFFunctionCount), 4, CFCount);
    HasEHTableField =
        ReadField(offsetof(coff_load_configuration32, GuardEHContinuationTable),
                  4, EHTableVA);
    HasEHCountField =
        ReadField(offsetof(coff_load_configuration32, GuardEHContinuationCount),
                  4, EHCount);
  }

  auto ValidateGuardTable = [&](uint64_t TableVA, uint64_t Count,
                                uint32_t Stride,
                                llvm::StringRef Name) -> llvm::Error {
    if (Count == 0)
      return llvm::Error::success();
    if (TableVA < ImageBase ||
        TableVA - ImageBase > std::numeric_limits<uint32_t>::max() ||
        Count > Binary.size() / Stride)
      return patchError(Name + " has an invalid pointer/count");
    uint32_t TableRVA = static_cast<uint32_t>(TableVA - ImageBase);
    uint64_t Bytes = Count * Stride;
    if (TableRVA >= SizeOfImage || Bytes > SizeOfImage - TableRVA)
      return patchError(Name + " leaves the image extent");
    auto Offset = rvaToFileOffset(PE, Binary, TableRVA, Bytes);
    if (!Offset)
      return patchError(Name + " is not file-backed");
    uint32_t Previous = 0;
    bool HavePrevious = false;
    for (uint64_t I = 0; I < Count; ++I) {
      uint32_t RVA = readLE<uint32_t>(Binary.data() + *Offset + I * Stride);
      if (!IsExecutableRVA(RVA))
        return patchError(Name + " contains a non-executable target");
      if (HavePrevious && RVA <= Previous)
        return patchError(Name + " is not strictly RVA-sorted");
      Previous = RVA;
      HavePrevious = true;
    }
    return llvm::Error::success();
  };

  if ((GuardFlags &
       uint32_t(llvm::COFF::GuardFlags::CF_FUNCTION_TABLE_PRESENT)) != 0) {
    if (!HasCFTableField || !HasCFCountField)
      return patchError("final Guard CF fields are truncated");
    uint32_t ExtraBytes =
        (GuardFlags &
         uint32_t(llvm::COFF::GuardFlags::CF_FUNCTION_TABLE_SIZE_MASK)) >>
        28;
    if (llvm::Error Err = ValidateGuardTable(CFTableVA, CFCount, 4 + ExtraBytes,
                                             "final Guard CF table"))
      return Err;
  }
  if ((GuardFlags &
       uint32_t(llvm::COFF::GuardFlags::EH_CONTINUATION_TABLE_PRESENT)) != 0) {
    if (!HasEHTableField || !HasEHCountField)
      return patchError("final Guard EH continuation fields are truncated");
    if (llvm::Error Err = ValidateGuardTable(
            EHTableVA, EHCount, 4, "final Guard EH continuation table"))
      return Err;
  }
  return llvm::Error::success();
}

llvm::Error validatePatchedCOFFImage(llvm::ArrayRef<uint8_t> Binary,
                                     Arch TargetArch) {
  return validatePatchedCOFFImage(Binary, TargetArch,
                                  /*RequireGeneratedExceptionDirectory=*/false);
}

llvm::Error validatePatchedCOFFImage(llvm::ArrayRef<uint8_t> Binary,
                                     Arch TargetArch,
                                     bool RequireGeneratedExceptionDirectory) {
  return validatePatchedCOFFImageImpl(Binary, TargetArch,
                                      RequireGeneratedExceptionDirectory,
                                      /*ExpectedExceptionDirectory=*/nullptr);
}

llvm::Error validatePatchedCOFFImage(
    llvm::ArrayRef<uint8_t> Binary, Arch TargetArch,
    bool RequireGeneratedExceptionDirectory,
    const COFFExceptionDirectoryUpdate &ExpectedExceptionDirectory) {
  return validatePatchedCOFFImageImpl(Binary, TargetArch,
                                      RequireGeneratedExceptionDirectory,
                                      &ExpectedExceptionDirectory);
}

} // namespace neverd
