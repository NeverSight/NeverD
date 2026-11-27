//===- ReleaseInstructions.cpp - Versioned CPython disk instructions -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Decode variable-width bytecode and wordcode with version-specific operand
/// assignments. Offsets and targets in the public report are always bytes.
//===----------------------------------------------------------------------===//

#include "Artifacts.h"

#include "neverd/analysis/PythonVersions.h"

#include "llvm/Support/Endian.h"

#include <array>
#include <limits>

using namespace llvm;
using namespace neverd::python;

namespace {
enum class Operand : uint8_t {
  None,
  Immediate,
  Prefix,
  Constant,
  Name,
  NameFlag,
  PackedName,
  Local,
  LocalPair,
  Free,
  Compare,
  Absolute,
  Forward,
  Backward
};
enum class Flow : uint8_t { Next, Jump, Stop, Handler, Dynamic };
struct Layout {
  const char *Name = nullptr;
  Operand Arg = Operand::None;
  Flow Transfer = Flow::Next;
  uint8_t Caches = 0;
};
struct Assignment {
  unsigned First, Last, Opcode;
  Layout Value;
};
constexpr Assignment Assignments[] = {
    {}, // Index zero identifies an unassigned physical opcode.
#define PYTHON_RELEASE_OPCODE(A, B, ID, NAME, ARG, FLOW, CACHES)               \
  {A, B, ID, {NAME, Operand::ARG, Flow::FLOW, CACHES}},
#include "ReleaseOpcodes.def"
#undef PYTHON_RELEASE_OPCODE
};
using OpcodeIndices = std::array<uint16_t, 256>;
static_assert(std::size(Assignments) <= UINT16_MAX);
constexpr auto Layouts = [] {
  std::array<OpcodeIndices, std::size(Releases)> Tables{};
  for (size_t Index = 1; Index < std::size(Assignments); ++Index) {
    const auto &A = Assignments[Index];
    for (size_t I = 0; I < std::size(Releases); ++I)
      if (Releases[I].Version >= A.First && Releases[I].Version <= A.Last)
        Tables[I][A.Opcode] = Index;
  }
  return Tables;
}();
const OpcodeIndices &layouts(unsigned Version) {
  for (size_t I = 0; I < std::size(Releases); ++I)
    if (Releases[I].Version == Version)
      return Layouts[I];
  llvm_unreachable("unknown CPython release");
}

void legacyLines(const Code &C, MutableArrayRef<InstructionRecord> Rows) {
  StringRef Table = C.Lines->Data;
  if (Table.size() % 2)
    malformed(C.Lines->DataOffset, "Truncated line number table");
  int64_t Line = C.FirstLine;
  size_t Offset = 0, Index = 0;
  for (size_t N = 0; N < Table.size(); N += 2) {
    Offset += uint8_t(Table[N]);
    if (Offset > C.Bytes->Data.size())
      malformed(C.Lines->DataOffset + N, "Line number offset leaves code");
    while (Index < Rows.size() && Rows[Index].PC < Offset)
      Rows[Index++].OriginalLine = Line;
    Line += C.Version >= 306 ? int(int8_t(Table[N + 1]))
                             : int(uint8_t(Table[N + 1]));
    if (Line < 0 || Line > INT32_MAX)
      malformed(C.Lines->DataOffset + N, "Line number out of range");
  }
  while (Index < Rows.size())
    Rows[Index++].OriginalLine = Line;
}

void checkIndex(const Code &C, const Layout &L, uint32_t Arg, size_t At) {
  auto Check = [&](uint32_t N, size_t Count) {
    if (N >= Count)
      malformed(At, "Instruction operand outside its code-object table");
  };
  switch (L.Arg) {
  case Operand::Constant:
    Check(Arg, C.Consts->Items.size());
    break;
  case Operand::Name:
    Check(Arg, C.Names->Items.size());
    break;
  case Operand::NameFlag:
    Check(Arg >> 1, C.Names->Items.size());
    break;
  case Operand::PackedName:
    Check(Arg >> 2, C.Names->Items.size());
    break;
  case Operand::LocalPair:
    Check(Arg >> 4, C.Locals);
    Check(Arg & 15, C.Locals);
    break;
  case Operand::Local:
    Check(Arg, C.Locals);
    break;
  case Operand::Free:
    if (C.LocalKinds) {
      Check(Arg, C.LocalKinds->Data.size());
      if (!(uint8_t(C.LocalKinds->Data[Arg]) & 0xc0))
        malformed(At,
                  "Closure operand does not designate a cell or free variable");
    } else
      Check(Arg, C.Free->Items.size() + C.Cells->Items.size());
    break;
  case Operand::Compare:
    if (C.Version >= 312)
      Check(Arg >> (C.Version == 312 ? 4 : 5), 6);
    else
      Check(Arg, C.Version >= 309 ? 6 : 11);
    break;
  default:
    break;
  }
}
} // namespace

void neverd::python::decodeRelease(Job &J, Function &F) {
  const Code &C = *F.Node.Body;
  const unsigned V = C.Version, Unit = V >= 306 ? 2 : 1;
  StringRef Bytes = C.Bytes->Data;
  const auto &Table = layouts(V);
  const size_t At = C.Bytes->DataOffset;
  if (Unit == 2)
    J.charge(J.Instructions, Bytes.size() / 2, limits::kInstructions, At);
  else if (Bytes.size() > 3 * (limits::kInstructions - J.Instructions))
    throw Failure{"budget_exceeded", At, "Bytecode exceeds instruction budget"};
  std::vector<int> Starts(Bytes.size() / Unit, -1);
  std::vector<std::optional<unsigned>> Lines;
  if (V >= 311)
    Lines = locationLines(J, C);
  std::optional<unsigned> SetLine;
  auto &Rows = F.DiskInstructions;
  for (unsigned PC = 0; PC < Bytes.size();) {
    unsigned Start = PC, OpcodePC, ID;
    uint64_t Arg = 0;
    const Layout *L;
    unsigned Prefixes = 0;
    do {
      if (PC == Bytes.size())
        malformed(At + PC, "Missing instruction after prefix");
      J.check(At + PC);
      OpcodePC = PC;
      ID = uint8_t(Bytes[PC++]);
      L = &Assignments[Table[ID]].Value;
      if (!L->Name)
        unsupported(At + OpcodePC, "Unknown or runtime-only disk opcode " +
                                       std::to_string(ID));
      bool HasArg = L->Arg != Operand::None;
      if (Prefixes && !HasArg)
        malformed(At + Start, "Prefix on an operand-free opcode");
      if (Unit == 2) {
        Arg = (Arg << 8) | uint8_t(Bytes[PC++]);
        if (!HasArg && Arg)
          malformed(At + Start,
                    "Unexpected operand on operand-free instruction");
      } else {
        J.charge(J.Instructions, 1, limits::kInstructions, At + OpcodePC);
        if (HasArg) {
          if (Bytes.size() - PC < 2)
            malformed(At + PC, "Truncated bytecode operand");
          Arg = (Arg << 16) | support::endian::read16le(Bytes.data() + PC);
          PC += 2;
        }
      }
      if (L->Arg == Operand::Prefix && ++Prefixes > (Unit == 2 ? 3u : 1u))
        malformed(At + Start, "Extended operand exceeds 32 bits");
    } while (L->Arg == Operand::Prefix);
    size_t CacheBytes = 2 * L->Caches;
    if (CacheBytes > Bytes.size() - PC)
      malformed(At + PC, "Truncated inline cache");
    if (Bytes.substr(PC, CacheBytes).find_first_not_of('\0') != StringRef::npos)
      unsupported(At + PC, "Populated cache is outside the disk profile");
    PC += CacheBytes;
    checkIndex(C, *L, Arg, At + Start);
    Starts[Start / Unit] = Rows.size();
    auto &Row = Rows.emplace_back(InstructionRecord{
        L->Name, At + Start, Start, PC, ID, static_cast<unsigned>(Arg)});
    if (V >= 311) {
      Row.OpcodePC = OpcodePC;
      Row.CacheUnits = L->Caches;
      Row.OriginalLine = Lines[Start / 2];
    } else if (V < 105) {
      if (Row.Mnemonic == "SET_LINENO")
        SetLine = Arg;
      Row.OriginalLine = SetLine;
    }
    Row.FallsThrough =
        L->Transfer == Flow::Next || L->Transfer == Flow::Handler;
    Row.Handler = L->Transfer == Flow::Handler;
    if (L->Arg == Operand::Absolute || L->Arg == Operand::Forward ||
        L->Arg == Operand::Backward) {
      int64_t Delta = int64_t(Arg) * (V >= 310 ? 2 : 1);
      int64_t Target =
          L->Arg == Operand::Absolute
              ? Delta
              : int64_t(PC) + (L->Arg == Operand::Backward ? -Delta : Delta);
      if (Target < 0 || uint64_t(Target) >= Bytes.size() || Target % Unit)
        malformed(At + Start, "Jump leaves code or enters an operand");
      Row.Target = Target;
    }
    if (Row.FallsThrough && PC == Bytes.size())
      malformed(At + Start, "Fallthrough leaves code object");
  }
  for (const auto &I : Rows)
    if (I.Target && Starts[*I.Target / Unit] < 0)
      malformed(I.Offset, "Jump enters an operand, prefix or cache");
  if (V >= 311)
    F.ExceptionRegions = exceptionRegions(J, C, Starts);
  else if (V >= 105)
    legacyLines(C, Rows);
  for (auto &I : Rows)
    I.Complete = Assignments[Table[I.Opcode]].Value.Transfer != Flow::Dynamic;
  F.Decoded = true;
}
