//===- BinaryComparison.cpp - Native comparison snapshots ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Builds compact, revision-bound snapshots from the SDK's shared native
/// traversal. Operand normalization consumes exact relocation occurrences and
/// lifted control-flow facts; numeric resemblance to an address is
/// insufficient.
//===----------------------------------------------------------------------===//

#include "BinaryComparison.h"

#include "InstructionFlow.h"
#include "SessionImpl.h"

#include "neverd/decode/InstructionRelocations.h"
#include "neverd/diff/StableHash.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/support/FilePath.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <map>
#include <set>
#include <stdexcept>

namespace neverd::sdk {
namespace {
using namespace diff;
namespace limits = neverd::limits::diff;

struct ReadInstruction {
  uint64_t Address = 0;
  std::vector<uint8_t> Bytes;
  std::string Mnemonic;
  std::string Operands;
  uint64_t Shape = 0;
  uint64_t Skeleton = 0;
  uint64_t AddressIndependentShape = 0;
  bool HasUnresolvedData = false;
  bool NoReturn = false;
  diff::InstructionFlow Flow = diff::InstructionFlow::None;
  std::optional<uint64_t> Target;
  std::optional<uint64_t> Callee;
  std::vector<std::pair<ReferenceKind, uint64_t>> References;
  std::vector<AddressReference> AddressReferences;
};

struct AddressKeys {
  const BinaryImage &Image;
  std::map<va_t, const Import *> Imports;
  std::map<va_t, const Symbol *> Data;
  std::set<va_t> Functions;
  uint64_t InputIdentity;

  AddressKeys(const BinaryImage &Image, llvm::ArrayRef<FuncInfo> Functions,
              uint64_t InputIdentity)
      : Image(Image), InputIdentity(InputIdentity) {
    for (const auto &I : Image.Imports)
      if (I.IATAddr)
        Imports.emplace(I.IATAddr, &I);
    for (const auto &S : Image.Symbols)
      if (!S.IsFunc && !S.IsSection && S.Origin == NameOrigin::Stated)
        Data.try_emplace(S.Addr, &S);
    for (const auto &F : Functions)
      this->Functions.insert(F.Entry);
  }

  std::pair<ReferenceKind, uint64_t> reference(va_t Address) const {
    StableHash H;
    if (const auto It = Imports.find(Address); It != Imports.end())
      return {ReferenceKind::Data, H.add("import")
                                       .add(It->second->Module)
                                       .add(It->second->Name)
                                       .add(It->second->Ordinal)
                                       .value()};
    if (Functions.contains(Address))
      return {ReferenceKind::Function, Address};
    if (const auto String = string(Address))
      return {ReferenceKind::String, *String};
    if (auto It = Data.upper_bound(Address); It != Data.begin()) {
      --It;
      const auto &S = *It->second;
      if (Address == S.Addr || Address - S.Addr < S.Size)
        return {ReferenceKind::Data,
                H.add("data").add(S.Name).add(Address - S.Addr).value()};
    }
    return {ReferenceKind::UnresolvedData, unresolved(Address)};
  }

  uint64_t unresolved(va_t Address) const {
    // An RVA is meaningful within one input. Layout changes can reuse it
    // for a different object, even when every instruction byte stayed equal.
    return StableHash()
        .add("unresolved_address")
        .add(InputIdentity)
        .add(Address - Image.Base)
        .value();
  }

  std::optional<uint64_t> string(va_t Address) const {
    const Segment *S = Image.getSegmentFor(Address);
    if (!S || S->isExecutable() || Address - S->VA >= S->Data.size())
      return {};
    const auto Bytes = llvm::ArrayRef(S->Data)
                           .drop_front(Address - S->VA)
                           .take_front(limits::kMaxDataReferenceBytes);
    size_t End = 0;
    while (End < Bytes.size() && Bytes[End] >= ' ' && Bytes[End] <= '~')
      ++End;
    if (End < limits::kMinStringReferenceBytes || End == Bytes.size() ||
        Bytes[End] != 0)
      return {};
    return StableHash().add(Bytes.take_front(End)).value();
  }
};

// Operand occurrences, not equality with some other operand's numeric value,
// decide address roles. All control-flow and literal-read targets come from
// the ordinary lift; in particular, literal operands can carry lossy addresses
// in decoder detail, while the lift retains their full signed PC displacement.
bool readAArch64Operands(
    const DecodedInsn &I, const neverd::sdk::InstructionFlow &Flow,
    llvm::function_ref<void(uint64_t)> Both,
    llvm::function_ref<void(uint64_t)> Scalar,
    llvm::function_ref<void(uint64_t, bool, unsigned)> Address,
    llvm::function_ref<void(uint64_t, unsigned)> AddressFragment) {
  const cs_aarch64 &A = I.Raw->detail->aarch64;
  Both(A.cc);
  Both(A.update_flags);
  Both(I.Raw->detail->writeback);
  Both(A.post_index);
  Both(A.op_count);
  bool Complete = true;
  for (unsigned N = 0; N < A.op_count; ++N) {
    const cs_aarch64_op &O = A.operands[N];
    Both(O.type);
    Both(O.vector_index);
    Both(O.vas);
    Both(O.shift.type);
    Both(O.shift.value);
    Both(O.ext);
    Both(O.is_vreg);
    Both(O.is_list_member);
    switch (O.type) {
    case AARCH64_OP_REG:
    case AARCH64_OP_MEM_REG:
      Both(O.reg);
      break;
    case AARCH64_OP_IMM:
    case AARCH64_OP_MEM_IMM:
      // TBZ/TBNZ also have a bit-index immediate. Only the final operand
      // denotes the transfer destination, even when both values happen to
      // agree.
      if (N + 1 == A.op_count && Flow.Target != InvalidVA &&
          static_cast<uint64_t>(O.imm) == Flow.Target)
        Address(Flow.Target, true, N);
      else if (I.Id == AARCH64_INS_ADR && N == 1 && A.op_count == 2)
        Address(static_cast<uint64_t>(O.imm), false, N);
      else if (I.Id == AARCH64_INS_ADRP && N == 1 && A.op_count == 2) {
        // ADRP materializes an address fragment. Keep the actual image page;
        // no object identity or ADRP+ADD pairing follows from this operand.
        AddressFragment(static_cast<uint64_t>(O.imm), N);
      } else
        Scalar(static_cast<uint64_t>(O.imm));
      break;
    case AARCH64_OP_CIMM:
    case AARCH64_OP_IMPLICIT_IMM_0:
      Scalar(static_cast<uint64_t>(O.imm));
      break;
    case AARCH64_OP_MEM:
      Both(O.mem.base);
      Both(O.mem.index);
      if (N == 1 && A.op_count == 2 && O.mem.base == AARCH64_REG_INVALID &&
          O.mem.index == AARCH64_REG_INVALID && Flow.Refs.size() == 1 &&
          Flow.Refs.front().Kind == "read")
        Address(Flow.Refs.front().To, false, N);
      else
        Scalar(static_cast<uint64_t>(O.mem.disp));
      break;
    case AARCH64_OP_FP:
      Scalar(std::bit_cast<uint64_t>(O.fp));
      break;
    case AARCH64_OP_SYSREG:
      Both(O.sysop.sub_type);
      Both(O.sysop.reg.raw_val);
      break;
    case AARCH64_OP_SYSIMM:
      Both(O.sysop.sub_type);
      Scalar(O.sysop.imm.raw_val);
      break;
    case AARCH64_OP_SYSALIAS:
      Both(O.sysop.sub_type);
      Both(O.sysop.alias.raw_val);
      break;
    case AARCH64_OP_IMM_RANGE:
      Scalar(O.imm_range.first);
      Scalar(O.imm_range.offset);
      break;
    case AARCH64_OP_PRED:
      Both(O.pred.reg);
      Both(O.pred.vec_select);
      Scalar(O.pred.imm_index);
      break;
    case AARCH64_OP_SME:
      Both(O.sme.type);
      Both(O.sme.tile);
      if (O.sme.type == AARCH64_SME_OP_TILE_VEC) {
        Both(O.sme.slice_reg);
        Both(O.sme.is_vertical);
        Both(O.sme.has_range_offset);
        if (O.sme.has_range_offset) {
          Scalar(O.sme.slice_offset.imm_range.first);
          Scalar(O.sme.slice_offset.imm_range.offset);
        } else
          Scalar(O.sme.slice_offset.imm);
      } else if (O.sme.type != AARCH64_SME_OP_TILE)
        Complete = false;
      break;
    default:
      Complete = false;
      break;
    }
  }
  return Complete;
}

ReadInstruction readInstruction(const BinaryImage &Image,
                                const AddressKeys &Keys, Decoder &D,
                                const DecodedInsn &I,
                                InstructionFlowScratch &Scratch,
                                va_t FunctionEntry) {
  ReadInstruction R;
  R.Address = I.Addr;
  R.Bytes.assign(I.Raw->bytes, I.Raw->bytes + I.Size);
  R.Mnemonic = I.Raw->mnemonic;
  R.Operands = I.Raw->op_str;
  const auto Flow = Scratch.summarize(Image, D, I);
  R.NoReturn = Flow.NoReturn;
  if (Flow.Target != InvalidVA)
    R.Target = Flow.Target;
  if (Flow.Kind == "call") {
    R.Flow = diff::InstructionFlow::Call;
    R.Callee = R.Target;
  } else if (Flow.Kind == "icall") {
    R.Flow = diff::InstructionFlow::IndirectCall;
  } else if (Flow.Kind == "jump" || Flow.Kind == "cjump") {
    R.Flow = Flow.Kind == "jump" ? diff::InstructionFlow::Jump
                                 : diff::InstructionFlow::Branch;
    if (R.Target && *R.Target != FunctionEntry &&
        Keys.Functions.contains(*R.Target))
      R.Callee = R.Target;
  } else if (Flow.Kind == "ijump") {
    R.Flow = diff::InstructionFlow::IndirectJump;
  } else if (Flow.Kind == "ret") {
    R.Flow = diff::InstructionFlow::Return;
  } else if (Flow.Kind == UnliftedFlow) {
    R.Flow = diff::InstructionFlow::Unknown;
  }
  for (const auto &Ref : Flow.Refs) {
    if ((Flow.Kind == "icall" || Flow.Kind == "ijump") &&
        Keys.Imports.contains(Ref.To)) {
      R.Callee = Ref.To;
      R.Target = Ref.To;
    }
    const auto Key = Keys.reference(Ref.To);
    if (Key.first != ReferenceKind::Function)
      R.References.push_back(Key);
  }
  StableHash Shape, Skeleton, AddressIndependentShape;
  Shape.add(R.Mnemonic);
  Skeleton.add(R.Mnemonic);
  AddressIndependentShape.add(R.Mnemonic);
  const auto Both = [&](uint64_t Value) {
    Shape.add(Value);
    Skeleton.add(Value);
    AddressIndependentShape.add(Value);
  };
  const auto Address = [&](uint64_t Value, bool Transfer, unsigned Operand) {
    AddressIndependentShape.add(hashText("address_operand")).add(Transfer);
    uint64_t Key = 0;
    if (Transfer && R.Callee && Keys.Functions.contains(Value))
      Key = hashText("callee");
    else if (Transfer && (R.Flow == diff::InstructionFlow::Jump ||
                          R.Flow == diff::InstructionFlow::Branch))
      Key =
          hashText("cfg_target"); // Destination is compared through CFG edges.
    else if (Transfer && Keys.Functions.contains(Value))
      Key = hashText("callee"); // Compared through function correspondence.
    else {
      const auto Ref = Keys.reference(Value);
      R.AddressReferences.push_back({Ref.second, Operand, Ref.first});
      Shape.add(static_cast<uint64_t>(Ref.first));
      Skeleton.add(static_cast<uint64_t>(Ref.first));
      if (Ref.first == ReferenceKind::Function) {
        Key = hashText("function_address"); // Compared through correspondence.
      } else if (Ref.first == ReferenceKind::UnresolvedData) {
        R.References.push_back(Ref);
        Shape.add(Ref.second);
        Skeleton.add(hashText("unresolved_address"));
        R.HasUnresolvedData = true;
        return;
      } else {
        R.References.push_back(Ref);
        Key = Ref.second;
      }
    }
    Shape.add(Key);
    Skeleton.add(Key);
  };
  const auto Scalar = [&](uint64_t Value) {
    Shape.add(Value);
    Skeleton.add(0);
    AddressIndependentShape.add(Value);
    R.References.emplace_back(ReferenceKind::Constant, Value);
  };
  const auto Relocs = instructionRelocations(Image, I);
  const auto Relocated = [&](unsigned Offset,
                             unsigned Width) -> std::optional<va_t> {
    for (const auto &Rel : Relocs.Addresses)
      if (Rel.FieldVA == I.Addr + Offset && Rel.Width == Width)
        return Rel.TargetVA;
    return {};
  };
  if (Image.Arch == Arch::AArch64) {
    const auto AddressFragment = [&](uint64_t Value, unsigned Operand) {
      Both(hashText("aarch64_image_page"));
      Shape.add(Keys.unresolved(Value));
      Skeleton.add(0);
      R.References.emplace_back(ReferenceKind::UnresolvedData,
                                Keys.unresolved(Value));
      R.AddressReferences.push_back(
          {Keys.unresolved(Value), Operand, ReferenceKind::UnresolvedData});
      R.HasUnresolvedData = true;
    };
    if (!readAArch64Operands(I, Flow, Both, Scalar, Address, AddressFragment))
      R.Flow = diff::InstructionFlow::Unknown;
  } else {
    const cs_x86 &X = I.Raw->detail->x86;
    for (uint8_t Prefix : X.prefix)
      Both(Prefix);
    Both(X.addr_size);
    Both(X.op_count);
    Both(X.xop_cc);
    Both(X.sse_cc);
    Both(X.avx_cc);
    Both(X.avx_sae);
    Both(X.avx_rm);
    for (unsigned N = 0; N < X.op_count; ++N) {
      const cs_x86_op &O = X.operands[N];
      Both(O.type);
      Both(O.avx_bcast);
      Both(O.avx_zero_opmask);
      // Direct transfer encoding width can change when the same blocks move.
      if (!(O.type == X86_OP_IMM && R.Target && Flow.Target != InvalidVA))
        Both(O.size);
      switch (O.type) {
      case X86_OP_REG:
        Both(O.reg);
        break;
      case X86_OP_IMM:
        if (R.Target && Flow.Target != InvalidVA &&
            static_cast<uint64_t>(O.imm) == Flow.Target)
          Address(Flow.Target, true, N);
        else if (auto A = Relocated(X.encoding.imm_offset, X.encoding.imm_size))
          Address(*A, false, N);
        else
          Scalar(static_cast<uint64_t>(O.imm));
        break;
      case X86_OP_MEM:
        Both(O.mem.segment);
        Both(O.mem.base);
        Both(O.mem.index);
        Both(O.mem.scale);
        if (O.mem.base == X86_REG_RIP || O.mem.base == X86_REG_EIP) {
          uint64_t A = I.Addr + I.Size + O.mem.disp;
          if (O.mem.base == X86_REG_EIP)
            A = static_cast<uint32_t>(A);
          Address(A, false, N);
        } else if (auto A =
                       Relocated(X.encoding.disp_offset, X.encoding.disp_size))
          Address(*A, false, N);
        else
          Scalar(static_cast<uint64_t>(O.mem.disp));
        break;
      default:
        // Unsupported operand classes retain all encoding evidence and are
        // explicitly incomplete, never an invented normalized instruction.
        R.Flow = diff::InstructionFlow::Unknown;
        Shape.add(R.Bytes);
        Skeleton.add(R.Bytes);
        AddressIndependentShape.add(R.Bytes);
        break;
      }
    }
  }
  if (R.Flow == diff::InstructionFlow::Unknown) {
    Shape.add(R.Bytes);
    Skeleton.add(R.Bytes);
    AddressIndependentShape.add(R.Bytes);
  }
  R.Shape = Shape.value();
  R.Skeleton = Skeleton.value();
  R.AddressIndependentShape = AddressIndependentShape.value();
  return R;
}

void appendFunction(ProgramBuilder &Builder, const FuncInfo &F,
                    llvm::StringRef Name, std::vector<ReadInstruction> &Rows,
                    const FunctionDecodeStatus &Status, const AddressKeys &Keys,
                    uint16_t Traits) {
  const NameSource Source =
      F.ResolvedOrigin == NameOrigin::Synthesized ? NameSource::Generated
      : F.ResolvedOrigin == NameOrigin::Analysis  ? NameSource::Signature
                                                  : NameSource::Symbol;
  Builder.beginFunction(F.Entry, F.Size, Name.str(), Source, Traits);
  llvm::sort(
      Rows, [](const auto &L, const auto &R) { return L.Address < R.Address; });
  Rows.erase(std::unique(Rows.begin(), Rows.end(),
                         [](const auto &L, const auto &R) {
                           return L.Address == R.Address;
                         }),
             Rows.end());
  bool Incomplete = Status.Incomplete || Status.Stopped || Rows.empty();
  std::map<va_t, uint32_t> RowAt, BlockAt;
  for (uint32_t I = 0; I < Rows.size(); ++I)
    RowAt.emplace(Rows[I].Address, I);
  std::set<va_t> Starts;
  Starts.insert(F.Entry);
  if (!Rows.empty())
    Starts.insert(Rows.front().Address);
  for (size_t I = 0; I < Rows.size(); ++I) {
    const auto &R = Rows[I];
    Incomplete |= R.Flow == diff::InstructionFlow::Unknown ||
                  (R.Flow == diff::InstructionFlow::IndirectJump && !R.Callee);
    if (R.Flow == diff::InstructionFlow::Branch ||
        R.Flow == diff::InstructionFlow::Jump) {
      if (R.Target && RowAt.contains(*R.Target))
        Starts.insert(*R.Target);
      else if (!R.Target || !Keys.Functions.contains(*R.Target))
        Incomplete = true;
    }
    if (I + 1 < Rows.size() &&
        (R.Address + R.Bytes.size() != Rows[I + 1].Address ||
         R.Flow == diff::InstructionFlow::Branch ||
         R.Flow == diff::InstructionFlow::Jump ||
         R.Flow == diff::InstructionFlow::IndirectJump ||
         R.Flow == diff::InstructionFlow::Return ||
         R.Flow == diff::InstructionFlow::Unknown || R.NoReturn))
      Starts.insert(Rows[I + 1].Address);
  }
  uint32_t Block = 0;
  std::vector<size_t> Ends;
  for (size_t I = 0; I < Rows.size(); ++I) {
    const auto &R = Rows[I];
    if (Starts.contains(R.Address)) {
      if (I)
        Ends.push_back(I - 1);
      Block = Builder.addBlock(R.Address);
      BlockAt.emplace(R.Address, Block);
    }
    Builder.addInstruction(R.Address, R.Bytes, R.Mnemonic, R.Shape, R.Skeleton,
                           R.Flow, R.Callee, R.Operands, R.Target,
                           R.AddressReferences, R.AddressIndependentShape,
                           R.HasUnresolvedData);
    for (const auto &[Kind, Key] : R.References)
      Builder.addReference(Kind, Key, Block);
  }
  if (!Rows.empty())
    Ends.push_back(Rows.size() - 1);
  for (uint32_t B = 0; B < Ends.size(); ++B) {
    const auto &R = Rows[Ends[B]];
    if (R.Target && BlockAt.contains(*R.Target) &&
        (R.Flow == diff::InstructionFlow::Branch ||
         R.Flow == diff::InstructionFlow::Jump))
      Builder.addEdge(B, BlockAt.at(*R.Target),
                      R.Flow == diff::InstructionFlow::Branch ? EdgeKind::Taken
                                                              : EdgeKind::Jump);
    if (!R.NoReturn && (R.Flow == diff::InstructionFlow::None ||
                        R.Flow == diff::InstructionFlow::Call ||
                        R.Flow == diff::InstructionFlow::IndirectCall ||
                        R.Flow == diff::InstructionFlow::Branch)) {
      const auto Next = BlockAt.find(R.Address + R.Bytes.size());
      if (Next != BlockAt.end())
        Builder.addEdge(B, Next->second,
                        R.Flow == diff::InstructionFlow::Branch
                            ? EdgeKind::NotTaken
                            : EdgeKind::Fallthrough);
      else
        Incomplete = true;
    }
  }
  if (Rows.size() == 1 && (Rows[0].Flow == diff::InstructionFlow::Jump ||
                           Rows[0].Flow == diff::InstructionFlow::IndirectJump))
    Builder.addTraits(TraitThunk);
  if (Incomplete)
    Builder.addTraits(TraitIncomplete);
}

void hashWord(llvm::SHA256 &Hash, uint64_t Value) {
  uint8_t Bytes[sizeof(Value)];
  for (unsigned I = 0; I < sizeof(Value); ++I)
    Bytes[I] = static_cast<uint8_t>(Value >> (I * kBitsPerByte));
  Hash.update(Bytes);
}

std::string byteOverlayIdentity(const patch::PatchOverlay &Overlay) {
  // The patch owner bounds both retained bytes and rows. Hash the effective
  // file bytes independently of edit kinds, history and row partitioning.
  // Data outside the selected function inventory still belongs to the input.
  llvm::SHA256 Hash;
  Hash.update("neverd.bindiff.byte-overlay-v1");
  const auto &Rows = Overlay.rows();
  for (auto Begin = Rows.begin(); Begin != Rows.end();) {
    auto End = Begin;
    uint64_t Next = Begin->first;
    do {
      Next += End->second.Current.size();
      ++End;
    } while (End != Rows.end() && End->first == Next);
    hashWord(Hash, Begin->first);
    hashWord(Hash, Next - Begin->first);
    for (auto It = Begin; It != End; ++It)
      Hash.update(It->second.Current);
    Begin = End;
  }
  return llvm::toHex(Hash.final(), true);
}

std::string analysisSectionIdentity(const Session &S) {
  // Section admission owns the byte/record bounds and guarantees the named
  // materialized section. Retain every addition, including data outside the
  // selected function inventory. Creation order is not part of that state.
  std::map<llvm::StringRef, const llvm::json::Object *> Records;
  for (const auto &Value : S.AnalysisSections) {
    const auto &Record = *Value.getAsObject();
    Records.emplace(*Record.getString("name"), &Record);
  }
  llvm::SHA256 Hash;
  Hash.update("neverd.bindiff.analysis-sections-v1");
  hashWord(Hash, Records.size());
  for (const auto &[Name, Record] : Records) {
    auto Canonical = *Record;
    auto &Functions = *Canonical.getArray("functions");
    std::sort(Functions.begin(), Functions.end(),
              [](const auto &A, const auto &B) {
                // The owner canonicalizes offsets and refuses overlapping
                // declarations.
                return *A.getAsObject()->getString("offset") <
                       *B.getAsObject()->getString("offset");
              });
    const auto Metadata = jsonToString(std::move(Canonical));
    hashWord(Hash, Metadata.size());
    Hash.update(Metadata);
    const auto *Section = S.Img.getSectionByName(Name);
    assert(Section && "owned analysis section must remain materialized");
    hashWord(Hash, Section->Data.size());
    Hash.update(Section->Data);
  }
  return llvm::toHex(Hash.final(), true);
}

std::string revision(const Program &P) {
  llvm::SHA256 Hash;
  const auto Word = [&](uint64_t V) { hashWord(Hash, V); };
  const auto Text = [&](llvm::StringRef T) {
    Word(T.size());
    Hash.update(T);
  };
  Text(P.Identity.SHA256);
  Text(P.Identity.Analysis);
  Text(P.Identity.Normalization);
  Text(P.Identity.ABI);
  Word(P.Identity.DiscoveryComplete);
  for (const auto &F : P.Functions) {
    Word(F.Address);
    Word(F.Size);
    Text(F.Name);
    Text(F.Module);
    Word(F.Traits);
    Word(static_cast<unsigned>(F.Source));
    Word(F.EntryBlock);
    for (const auto &B : P.blocks(F)) {
      Word(B.Address);
      Word(B.InstructionCount);
      for (const auto &I : P.instructions(B)) {
        Word(I.Address);
        Word(I.Shape);
        Word(I.Skeleton);
        Word(I.AddressIndependentShape);
        Word(I.HasUnresolvedData);
        Word(I.Callee);
        Word(I.UnresolvedCallee);
        Word(I.AddressReferenceCount);
        for (const auto &Address : P.addressReferences(I)) {
          Word(Address.Key);
          Word(Address.Operand);
          Word(static_cast<unsigned>(Address.Kind));
        }
        Word(static_cast<unsigned>(I.Flow));
        Text(P.mnemonic(I));
        Text(I.Operands);
        Word(I.Size);
        Hash.update(P.bytes(I));
      }
    }
    for (const auto &E : P.edges(F)) {
      Word(E.From);
      Word(E.To);
      Word(static_cast<unsigned>(E.Kind));
    }
    for (const auto &R : P.references(F)) {
      Word(static_cast<unsigned>(R.Kind));
      Word(R.Key);
      Word(R.Block);
    }
  }
  return llvm::toHex(Hash.final(), true);
}
} // namespace

std::optional<ComparisonInput>
readComparisonInputImpl(Session &S, const ComparisonInputOptions &Options,
                        const diff::ComparisonControl &Control,
                        std::string &Error, bool Serial) {
  const bool X86 = S.Img.Arch == Arch::X64 || S.Img.Arch == Arch::X86;
  const bool AArch64 = S.Img.Arch == Arch::AArch64;
  const auto &Profile = Options.Profile;
  if (Profile != limits::kDefaultNativeProfile && Profile != "native-x86-v1" &&
      Profile != "native-aarch64-v1") {
    Error = "unsupported comparison profile: " + Profile;
    return {};
  }
  if (S.Loaded && AArch64 && S.Img.Bits != Bitness::Bits64) {
    Error = "native-aarch64-v1 requires 64-bit AArch64 inputs";
    return {};
  }
  if (!S.Loaded || !(X86 || AArch64) || (Profile == "native-x86-v1" && !X86) ||
      (Profile == "native-aarch64-v1" && !AArch64)) {
    Error = Profile + " does not support the loaded processor";
    return {};
  }
  if (!S.synchronizeFunctions()) {
    Error = "cannot synchronize functions";
    return {};
  }
  if (Control.Progress)
    Control.Progress("discovery", 0, S.Functions.size());
  if (Options.Discover && !Control.stop()) {
    const auto Stop = [](void *Context) -> int {
      return static_cast<const diff::ComparisonControl *>(Context)
          ->stop()
          .has_value();
    };
    const int Discovered = neverd_session_discover_functions_cancellable(
        &S, Stop, const_cast<diff::ComparisonControl *>(&Control));
    if (Discovered == -1) {
      Error = S.LastError;
      return {};
    }
  }
  ProgramIdentity Identity;
  Identity.Path =
      pathToUTF8(S.PackedProject ? S.PackedProject->Source : S.FilePath);
  if (S.PackedProject) {
    Identity.ProjectInputName = S.PackedProject->InputName;
    Identity.ProjectStateSHA256 = S.PackedProject->StateSHA256;
  }
  Identity.SHA256 = inputSha256(S);
  StableHash AddressInputIdentity;
  AddressInputIdentity.add(Identity.SHA256);
  // The same file can supply different raw slices or image projections.
  // Bind the effective mapping, while allowing a pure image-base relocation.
  AddressInputIdentity.add("native-mapping-v1").add(S.Img.Segments.size());
  for (const auto &Segment : S.Img.Segments)
    AddressInputIdentity.add(Segment.VA - S.Img.Base)
        .add(Segment.Size)
        .add(Segment.FileOff)
        .add(Segment.FileSz)
        .add(static_cast<uint64_t>(Segment.Flags));
  Identity.Format = S.Img.isELF()     ? "elf"
                    : S.Img.isCOFF()  ? "coff"
                    : S.Img.isMachO() ? "macho"
                                      : "raw";
  Identity.Architecture = AArch64                   ? "aarch64"
                          : S.Img.Arch == Arch::X64 ? "x86-64"
                                                    : "x86";
  Identity.Bits = S.Img.getPointerSize() * kBitsPerByte;
  Identity.Endian = "little";
  Identity.ABI = std::to_string(static_cast<unsigned>(S.Img.abiFormat())) +
                 ":" + Identity.Architecture;
  if (AArch64 && S.Img.isMachO())
    Identity.ABI += ":macho-subtype:" + llvm::utohexstr(S.Img.MachOCPUSubtype);
  Identity.Normalization =
      (AArch64 ? NativeAArch64Normalization : NativeX86Normalization).str();
  Identity.Analysis = std::string(VersionString) + ":sdk-native-decode-v5:" +
                      (Options.Discover ? "discover" : "known-functions");
  if (S.PackedProject)
    Identity.Analysis += ":nddb-v1:" + S.PackedProject->StateSHA256;
  const char *LoadOptions = neverd_session_load_options_json(&S);
  if (LoadOptions) {
    Identity.Analysis += ":" + std::string(LoadOptions);
    neverd_free_string(LoadOptions);
  }
  if (S.Img.MachOSharedCache) {
    const auto Cache = jsonToString(sharedCacheIdentity(S.Img));
    Identity.Analysis += ":" + Cache;
    AddressInputIdentity.add("shared-cache-v1").add(Cache);
  }
  if (!S.BytePatches.empty()) {
    const auto Overlay = byteOverlayIdentity(S.BytePatches);
    Identity.Analysis += ":byte-overlay-v1:" + Overlay;
    AddressInputIdentity.add("byte-overlay-v1").add(Overlay);
  }
  if (!S.AnalysisSections.empty()) {
    const auto Sections = analysisSectionIdentity(S);
    Identity.Analysis += ":analysis-sections-v1:" + Sections;
    AddressInputIdentity.add("analysis-sections-v1").add(Sections);
  }
  Identity.ImageBase = S.Img.Base;
  Identity.EntryPoint = S.Img.Entry;
  Identity.DiscoveryComplete = S.DiscoveredFunctions.has_value() &&
                               !Options.Begin && !Options.End &&
                               Options.Function.empty();
  ProgramBuilder Builder(Identity);
  if (!Identity.DiscoveryComplete)
    Builder.warn("Function inventory is partial or filtered; unmatched does "
                 "not establish addition or deletion.");
  if (const auto Stopped = Control.stop()) {
    ComparisonInput Result;
    Result.Stopped = Stopped;
    Result.StoppedPhase = Options.Discover ? "discovery" : "decode";
    Result.Program = Builder.finish();
    Result.Program.Identity.DiscoveryComplete = false;
    Result.Program.Warnings.push_back(
        "Snapshot extraction stopped before decoding; function inventory is "
        "incomplete.");
    Result.Program.Identity.Revision = revision(Result.Program);
    return Result;
  }
  const AddressKeys Keys(S.Img, S.Functions, AddressInputIdentity.value());
  const libc::NoReturnTargetIndex NoReturnTargets(S.Img);
  std::map<va_t, const FuncInfo *> Functions;
  for (const auto &F : S.Functions)
    if ((!Options.Begin || F.Entry >= *Options.Begin) &&
        (!Options.End || F.Entry < *Options.End) &&
        (Options.Function.empty() || F.Name == Options.Function))
      Functions.emplace(F.Entry, &F);
  std::map<va_t, const Import *> Imports = Keys.Imports;
  std::set<va_t> Exported;
  for (const auto &E : S.Img.Exports)
    Exported.insert(E.Addr);
  std::atomic<uint64_t> Instructions{0};
  std::atomic<uint64_t> Memory{0};
  std::atomic<bool> Exhausted{false};
  const auto Stop = [&] {
    return Exhausted.load() || Control.stop().has_value();
  };
  ComparisonInput Result;
  uint64_t Done = 0;
  auto It = Functions.begin();
  while (It != Functions.end() && !Stop()) {
    FunctionPage Page;
    Page.Entries.assign(Keys.Functions.begin(), Keys.Functions.end());
    const unsigned PageLimit = Serial ? 1 : limits::kDecodePageFunctions;
    for (unsigned I = 0; I < PageLimit && It != Functions.end() &&
                         Done + Page.Functions.size() < Options.MaxFunctions;
         ++I, ++It) {
      Page.Functions.push_back(It->second);
    }
    if (Page.Functions.empty()) {
      Exhausted = true;
      break;
    }
    std::vector<std::vector<ReadInstruction>> Slots(Page.Functions.size());
    std::vector<FunctionDecodeStatus> Status;
    if (!decodeFunctions(
            S.Img, Page,
            [&](size_t Index, Decoder &D, const DecodedInsn &I,
                InstructionFlowScratch &Scratch) {
              if (Instructions.fetch_add(1) >= Options.MaxInstructions) {
                Exhausted = true;
                return;
              }
              auto R = readInstruction(S.Img, Keys, D, I, Scratch,
                                       Page.Functions[Index]->Entry);
              // Charge both transient decode evidence and retained
              // feature/model storage. The sessions' loaded images are owned by
              // the caller.
              const uint64_t Charge =
                  sizeof(ReadInstruction) + sizeof(Instruction) +
                  R.Bytes.size() * 2 + R.Mnemonic.size() +
                  R.Operands.size() * 2 +
                  R.AddressReferences.size() *
                      (sizeof(AddressReference) * 2 + sizeof(Reference)) +
                  R.References.size() *
                      (sizeof(Reference) +
                       sizeof(std::pair<ReferenceKind, uint64_t>));
              if (Memory.fetch_add(Charge) + Charge > Options.MaxMemoryBytes) {
                Exhausted = true;
                return;
              }
              Slots[Index].push_back(std::move(R));
            },
            Stop, &Status, &NoReturnTargets)) {
      Error = "failed to initialize the native decoder";
      return {};
    }
    for (size_t I = 0; I < Page.Functions.size(); ++I) {
      const auto &F = *Page.Functions[I];
      while (!Imports.empty() && Imports.begin()->first < F.Entry) {
        const auto [Address, Import] = *Imports.begin();
        Builder.beginFunction(Address, 0, Import->Name, NameSource::Import,
                              TraitImport, Import->Module);
        Imports.erase(Imports.begin());
      }
      Imports.erase(F.Entry);
      uint16_t Traits = Exported.contains(F.Entry) ? TraitExported : 0;
      if (F.Entry == S.Img.Entry)
        Traits |= TraitEntryPoint;
      if (const auto A = S.FunctionAttributeEdits.find(F.Entry);
          A != S.FunctionAttributeEdits.end() &&
          A->second.Library.value_or(false))
        Traits |= TraitLibrary;
      llvm::StringRef Name = S.OriginalNames.at(F.Entry);
      if (Name == F.LinkageName)
        Name = functionNameBeforeNumbering(S.Img, F.Entry, Name);
      if (F.Name != Name) {
        const uint64_t Charge =
            sizeof(decltype(Result.DisplayNames)::value_type) + F.Name.size();
        if (Memory.fetch_add(Charge) + Charge > Options.MaxMemoryBytes) {
          Exhausted = true;
          break;
        }
        Result.DisplayNames.emplace(F.Entry, F.Name);
      }
      appendFunction(Builder, F, Name, Slots[I], Status[I], Keys, Traits);
    }
    Done += Page.Functions.size();
    if (Control.Progress)
      Control.Progress("decode", Done, Functions.size());
  }
  for (const auto &[Address, Import] : Imports)
    Builder.beginFunction(Address, 0, Import->Name, NameSource::Import,
                          TraitImport, Import->Module);
  Result.Program = Builder.finish();
  if (auto State = Control.stop())
    Result.Stopped = State;
  else if (Exhausted)
    Result.Stopped = ComparisonState::ResourceLimited;
  if (Result.Stopped) {
    Result.StoppedPhase = "decode";
    Result.Program.Identity.DiscoveryComplete = false;
    Result.Program.Warnings.push_back(
        "Snapshot extraction stopped; retained instructions and functions are "
        "partial.");
  }
  if (const auto Problem = Result.Program.validate()) {
    Error = *Problem;
    return {};
  }
  Result.Program.Identity.Revision = revision(Result.Program);
  return Result;
}
std::optional<ComparisonInput>
readComparisonInput(Session &S, const ComparisonInputOptions &Options,
                    const diff::ComparisonControl &Control,
                    std::string &Error) {
  auto Result = readComparisonInputImpl(S, Options, Control, Error, false);
  if (Result && Result->Stopped == ComparisonState::ResourceLimited &&
      !Control.stop()) {
    // A parallel stop can interrupt different slots on different schedules.
    // Rebuild the bounded address-order prefix serially before publishing it.
    // Free the first attempt before retaining any of the replacement snapshot.
    Result.reset();
    Result = readComparisonInputImpl(S, Options, Control, Error, true);
  }
  if (Result && Options.DataFlow.Enabled)
    Result->Program.DataFlow =
        readComparisonDataFlow(S, Result->Program, Options.DataFlow, Control);
  return Result;
}
} // namespace neverd::sdk
