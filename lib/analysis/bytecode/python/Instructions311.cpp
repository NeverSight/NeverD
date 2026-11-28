//===- Instructions311.cpp - CPython 3.11 disk wordcode -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Decode the cache-bearing disk layout, line positions and exception regions
/// independently of the 3.10 source emitter. PCs are bytes within co_code.
//===----------------------------------------------------------------------===//

#include "Artifacts.h"

#include <array>
#include <limits>

using namespace llvm;
using namespace neverd::python;

namespace {
enum class Operand {
  None,
  Immediate,
  Forward,
  Backward,
  Constant,
  Name,
  Global,
  Local,
  Closure,
  Compare,
  Binary,
  Bit,
  UpToTwo,
  Resume,
  FormatFlags,
  FunctionFlags,
  StackDepth,
  Slice
};
enum class Flow { Next, Jump, Stop };
struct Layout {
  const char *Name = nullptr;
  Operand Domain;
  Flow Successor;
  unsigned Caches;
};
constexpr auto Layouts = [] {
  std::array<Layout, 256> Table{};
#define PYTHON_DISK_OPCODE(ID, NAME, DOMAIN, FLOW, CACHES)                     \
  Table[ID] = {#NAME, Operand::DOMAIN, Flow::FLOW, CACHES};
#include "Opcodes311.def"
#undef PYTHON_DISK_OPCODE
  return Table;
}();

// Both formats use six payload bits. Locations put the low group first.
// The exception table puts the high group first and marks each record start.
class TableReader {
  const Object &Table;
  size_t Position = 0;

public:
  explicit TableReader(const Object &Table) : Table(Table) {}
  bool empty() const { return Position == Table.Data.size(); }
  size_t offset() const { return Table.DataOffset + Position; }
  uint8_t byte(bool Marker = false) {
    if (empty())
      malformed(offset(), "Truncated code-object table");
    unsigned Value = uint8_t(Table.Data[Position++]);
    if (bool(Value & 128) != Marker)
      malformed(offset() - 1, "Invalid code-table marker");
    return Value;
  }
  uint32_t number(bool LowFirst, bool Marker = false) {
    uint64_t Value = 0;
    for (unsigned Shift = 0; Shift <= 30; Shift += 6) {
      unsigned B = byte(Marker && !Shift);
      if (LowFirst)
        Value |= uint64_t(B & 63) << Shift;
      else
        Value = (Value << 6) | (B & 63);
      if (!(B & 64) && Value <= UINT32_MAX)
        return Value;
    }
    malformed(offset(), "Code-table integer is out of range");
  }
};

} // namespace

std::vector<std::optional<unsigned>>
neverd::python::locationLines(Job &J, const Code &C) {
  const size_t Units = C.Bytes->Data.size() / 2;
  std::vector<std::optional<unsigned>> Lines;
  Lines.reserve(Units);
  TableReader R(*C.Lines);
  int64_t Line = C.FirstLine;
  while (!R.empty()) {
    J.check(R.offset());
    unsigned Header = R.byte(true);
    unsigned Kind = (Header >> 3) & 15, Count = (Header & 7) + 1;
    if (Count > Units - Lines.size())
      malformed(R.offset(), "Location span leaves code");
    if (Kind < 10)
      R.byte();
    else if (Kind < 13) {
      Line += Kind - 10;
      R.byte();
      R.byte();
    } else if (Kind != 15) {
      unsigned Delta = R.number(true);
      Line += (Delta & 1) ? -int64_t(Delta >> 1) : int64_t(Delta >> 1);
      if (Kind == 14) {
        R.number(true);
        R.number(true);
        R.number(true);
      }
    }
    if (Line < 0 || Line > INT32_MAX)
      malformed(R.offset(), "Invalid location line number");
    Lines.insert(Lines.end(), Count,
                 Kind == 15 ? std::nullopt : std::optional<unsigned>(Line));
  }
  if (Lines.size() != Units)
    malformed(R.offset(), "Incomplete location table");
  return Lines;
}

json::Array neverd::python::exceptionRegions(Job &J, const Code &C,
                                             ArrayRef<int> Starts) {
  TableReader R(*C.Exceptions);
  json::Array Regions;
  unsigned Previous = 0;
  while (!R.empty()) {
    size_t At = R.offset();
    J.check(At);
    unsigned Start = R.number(false, true), Length = R.number(false),
             Target = R.number(false);
    unsigned Flags = R.number(false);
    if (Start >= Starts.size() || !Length || Length > Starts.size() - Start ||
        Start < Previous || Starts[Start] < 0 || Target >= Starts.size() ||
        Starts[Target] < 0 ||
        (Start + Length < Starts.size() && Starts[Start + Length] < 0) ||
        (Flags >> 1) > C.Stack)
      malformed(At, "Invalid protected range, handler or unwind depth");
    Previous = Start;
    Regions.emplace_back(json::Object{{"start", Start * 2},
                                      {"end", 2 * (Start + Length)},
                                      {"target", Target * 2},
                                      {"depth", Flags >> 1},
                                      {"lasti", bool(Flags & 1)},
                                      {"offset", At}});
  }
  return Regions;
}
void neverd::python::decode311(Job &J, Function &F) {
  const Code &C = *F.Node.Body;
  StringRef Bytes = C.Bytes->Data;
  J.charge(J.Instructions, Bytes.size() / 2, limits::kInstructions,
           C.Bytes->DataOffset);
  auto Lines = locationLines(J, C);
  std::vector<int> Starts(Bytes.size() / 2, -1);
  SmallVector<unsigned> Targets;
  std::vector<InstructionRecord> Rows;
  for (unsigned PC = 0; PC < Bytes.size();) {
    unsigned Start = PC, ID;
    uint32_t Arg = 0;
    do {
      if (PC == Bytes.size() || PC - Start > 6)
        malformed(C.Bytes->DataOffset + Start, "Invalid EXTENDED_ARG chain");
      ID = uint8_t(Bytes[PC]);
      Arg = (Arg << 8) | uint8_t(Bytes[PC + 1]);
      PC += 2;
    } while (ID == 144);
    size_t At = C.Bytes->DataOffset + Start;
    const Layout &L = Layouts[ID];
    if (!L.Name || !ID)
      unsupported(
          At, "Unknown, specialized or misplaced CPython 3.11 disk opcode " +
                  std::to_string(ID));
    unsigned OpcodePC = PC - 2;
    size_t CacheBytes = 2 * L.Caches;
    if (CacheBytes > Bytes.size() - PC)
      malformed(At, "Truncated inline cache");
    if (Bytes.substr(PC, CacheBytes).find_first_not_of('\0') != StringRef::npos)
      unsupported(At, "Populated inline cache is outside the on-disk profile");
    PC += CacheBytes;
    std::optional<unsigned> Target;
    bool Valid = true;
    switch (L.Domain) {
    case Operand::None:
      Valid = !Arg && Start == OpcodePC;
      break;
    case Operand::Constant:
      Valid = Arg < C.Consts->Items.size();
      break;
    case Operand::Name:
      Valid = Arg < C.Names->Items.size();
      break;
    case Operand::Global:
      Valid = (Arg >> 1) < C.Names->Items.size();
      break;
    case Operand::Compare:
      Valid = Arg < 6;
      break;
    case Operand::Local:
    case Operand::Closure:
      Valid = Arg < C.LocalKinds->Data.size() &&
              (uint8_t(C.LocalKinds->Data[Arg]) &
               (L.Domain == Operand::Local ? 0x20 : 0xc0));
      break;
    case Operand::Forward:
    case Operand::Backward: {
      int64_t Distance = int64_t(Arg) * 2;
      int64_t To = int64_t(OpcodePC) + 2 +
                   (L.Domain == Operand::Backward ? -Distance : Distance);
      if (To < 0 || uint64_t(To) >= Bytes.size())
        malformed(At, "Jump leaves code object");
      Target = To;
      Targets.push_back(*Target);
      break;
    }
    case Operand::Binary:
      Valid = Arg < 26;
      break; // Thirteen binary operations and their in-place forms.
    case Operand::Bit:
      Valid = Arg <= 1;
      break;
    case Operand::UpToTwo:
      Valid = Arg <= 2;
      break;
    case Operand::Resume:
      Valid = Arg <= 3;
      break;
    case Operand::FormatFlags:
      Valid = Arg <= 7;
      break;
    case Operand::FunctionFlags:
      Valid = Arg <= 15;
      break;
    case Operand::StackDepth:
      Valid = Arg >= 1 && Arg <= C.Stack;
      break;
    case Operand::Slice:
      Valid = Arg == 2 || Arg == 3;
      break;
    case Operand::Immediate:
      break;
    }
    if (!Valid)
      malformed(At, "Invalid CPython 3.11 instruction operand");
    Starts[Start / 2] = Rows.size();
    if (L.Successor == Flow::Next && PC == Bytes.size())
      malformed(At, "Fallthrough leaves code object");
    auto &Row =
        Rows.emplace_back(InstructionRecord{L.Name, At, Start, PC, ID, Arg});
    Row.Target = Target;
    Row.OriginalLine = Lines[Start / 2];
    Row.OpcodePC = OpcodePC;
    Row.CacheUnits = L.Caches;
    Row.Complete = true;
    Row.FallsThrough = L.Successor == Flow::Next;
  }
  for (unsigned PC : Targets)
    if (Starts[PC / 2] < 0)
      malformed(C.Bytes->DataOffset + PC,
                "Jump enters inline cache or prefixed operand");
  F.ExceptionRegions = exceptionRegions(J, C, Starts);
  F.DiskInstructions = std::move(Rows);
  F.Decoded = true;
}
