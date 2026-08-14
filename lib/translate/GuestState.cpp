//===- GuestState.cpp - Architecture-neutral guest machine state ---------===//

#include "neverd/translate/GuestState.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Errc.h"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <type_traits>
#include <utility>

namespace neverd::translate {
namespace {

// GuestState wire v1 baseline schema.  These tables are immutable: adding or
// changing baseline state requires a new wire version and an explicit upgrader.
// Optional machine state remains representable through stable extension IDs.
constexpr RegisterDescription GuestStateWireV1X86_64Registers[] = {
    {0, "rax", 64, RegisterKind::General},
    {1, "rcx", 64, RegisterKind::General},
    {2, "rdx", 64, RegisterKind::General},
    {3, "rbx", 64, RegisterKind::General},
    {4, "rsp", 64, RegisterKind::StackPointer},
    {5, "rbp", 64, RegisterKind::General},
    {6, "rsi", 64, RegisterKind::General},
    {7, "rdi", 64, RegisterKind::General},
    {8, "r8", 64, RegisterKind::General},
    {9, "r9", 64, RegisterKind::General},
    {10, "r10", 64, RegisterKind::General},
    {11, "r11", 64, RegisterKind::General},
    {12, "r12", 64, RegisterKind::General},
    {13, "r13", 64, RegisterKind::General},
    {14, "r14", 64, RegisterKind::General},
    {15, "r15", 64, RegisterKind::General},
    {16, "rip", 64, RegisterKind::ProgramCounter},
    {17, "rflags", 64, RegisterKind::Flags},
    {18, "mxcsr", 32, RegisterKind::FloatingPointControl},
    {32, "xmm0", 128, RegisterKind::Vector},
    {33, "xmm1", 128, RegisterKind::Vector},
    {34, "xmm2", 128, RegisterKind::Vector},
    {35, "xmm3", 128, RegisterKind::Vector},
    {36, "xmm4", 128, RegisterKind::Vector},
    {37, "xmm5", 128, RegisterKind::Vector},
    {38, "xmm6", 128, RegisterKind::Vector},
    {39, "xmm7", 128, RegisterKind::Vector},
    {40, "xmm8", 128, RegisterKind::Vector},
    {41, "xmm9", 128, RegisterKind::Vector},
    {42, "xmm10", 128, RegisterKind::Vector},
    {43, "xmm11", 128, RegisterKind::Vector},
    {44, "xmm12", 128, RegisterKind::Vector},
    {45, "xmm13", 128, RegisterKind::Vector},
    {46, "xmm14", 128, RegisterKind::Vector},
    {47, "xmm15", 128, RegisterKind::Vector},
};

constexpr RegisterDescription GuestStateWireV1X86_32Registers[] = {
    {0, "eax", 32, RegisterKind::General},
    {1, "ecx", 32, RegisterKind::General},
    {2, "edx", 32, RegisterKind::General},
    {3, "ebx", 32, RegisterKind::General},
    {4, "esp", 32, RegisterKind::StackPointer},
    {5, "ebp", 32, RegisterKind::General},
    {6, "esi", 32, RegisterKind::General},
    {7, "edi", 32, RegisterKind::General},
    {8, "eip", 32, RegisterKind::ProgramCounter},
    {9, "eflags", 32, RegisterKind::Flags},
    {10, "mxcsr", 32, RegisterKind::FloatingPointControl},
    {32, "xmm0", 128, RegisterKind::Vector},
    {33, "xmm1", 128, RegisterKind::Vector},
    {34, "xmm2", 128, RegisterKind::Vector},
    {35, "xmm3", 128, RegisterKind::Vector},
    {36, "xmm4", 128, RegisterKind::Vector},
    {37, "xmm5", 128, RegisterKind::Vector},
    {38, "xmm6", 128, RegisterKind::Vector},
    {39, "xmm7", 128, RegisterKind::Vector},
};

constexpr RegisterDescription GuestStateWireV1AArch64Registers[] = {
    {0, "x0", 64, RegisterKind::General},
    {1, "x1", 64, RegisterKind::General},
    {2, "x2", 64, RegisterKind::General},
    {3, "x3", 64, RegisterKind::General},
    {4, "x4", 64, RegisterKind::General},
    {5, "x5", 64, RegisterKind::General},
    {6, "x6", 64, RegisterKind::General},
    {7, "x7", 64, RegisterKind::General},
    {8, "x8", 64, RegisterKind::General},
    {9, "x9", 64, RegisterKind::General},
    {10, "x10", 64, RegisterKind::General},
    {11, "x11", 64, RegisterKind::General},
    {12, "x12", 64, RegisterKind::General},
    {13, "x13", 64, RegisterKind::General},
    {14, "x14", 64, RegisterKind::General},
    {15, "x15", 64, RegisterKind::General},
    {16, "x16", 64, RegisterKind::General},
    {17, "x17", 64, RegisterKind::General},
    {18, "x18", 64, RegisterKind::System},
    {19, "x19", 64, RegisterKind::General},
    {20, "x20", 64, RegisterKind::General},
    {21, "x21", 64, RegisterKind::General},
    {22, "x22", 64, RegisterKind::General},
    {23, "x23", 64, RegisterKind::General},
    {24, "x24", 64, RegisterKind::General},
    {25, "x25", 64, RegisterKind::General},
    {26, "x26", 64, RegisterKind::General},
    {27, "x27", 64, RegisterKind::General},
    {28, "x28", 64, RegisterKind::General},
    {29, "x29", 64, RegisterKind::General},
    {30, "x30", 64, RegisterKind::Link},
    {31, "sp", 64, RegisterKind::StackPointer},
    {32, "pc", 64, RegisterKind::ProgramCounter},
    {33, "nzcv", 32, RegisterKind::Flags},
    {34, "fpcr", 32, RegisterKind::FloatingPointControl},
    {35, "fpsr", 32, RegisterKind::FloatingPointControl},
    {64, "v0", 128, RegisterKind::Vector},
    {65, "v1", 128, RegisterKind::Vector},
    {66, "v2", 128, RegisterKind::Vector},
    {67, "v3", 128, RegisterKind::Vector},
    {68, "v4", 128, RegisterKind::Vector},
    {69, "v5", 128, RegisterKind::Vector},
    {70, "v6", 128, RegisterKind::Vector},
    {71, "v7", 128, RegisterKind::Vector},
    {72, "v8", 128, RegisterKind::Vector},
    {73, "v9", 128, RegisterKind::Vector},
    {74, "v10", 128, RegisterKind::Vector},
    {75, "v11", 128, RegisterKind::Vector},
    {76, "v12", 128, RegisterKind::Vector},
    {77, "v13", 128, RegisterKind::Vector},
    {78, "v14", 128, RegisterKind::Vector},
    {79, "v15", 128, RegisterKind::Vector},
    {80, "v16", 128, RegisterKind::Vector},
    {81, "v17", 128, RegisterKind::Vector},
    {82, "v18", 128, RegisterKind::Vector},
    {83, "v19", 128, RegisterKind::Vector},
    {84, "v20", 128, RegisterKind::Vector},
    {85, "v21", 128, RegisterKind::Vector},
    {86, "v22", 128, RegisterKind::Vector},
    {87, "v23", 128, RegisterKind::Vector},
    {88, "v24", 128, RegisterKind::Vector},
    {89, "v25", 128, RegisterKind::Vector},
    {90, "v26", 128, RegisterKind::Vector},
    {91, "v27", 128, RegisterKind::Vector},
    {92, "v28", 128, RegisterKind::Vector},
    {93, "v29", 128, RegisterKind::Vector},
    {94, "v30", 128, RegisterKind::Vector},
    {95, "v31", 128, RegisterKind::Vector},
};

constexpr RegisterDescription GuestStateWireV1ARM32Registers[] = {
    {0, "r0", 32, RegisterKind::General},
    {1, "r1", 32, RegisterKind::General},
    {2, "r2", 32, RegisterKind::General},
    {3, "r3", 32, RegisterKind::General},
    {4, "r4", 32, RegisterKind::General},
    {5, "r5", 32, RegisterKind::General},
    {6, "r6", 32, RegisterKind::General},
    {7, "r7", 32, RegisterKind::General},
    {8, "r8", 32, RegisterKind::General},
    {9, "r9", 32, RegisterKind::General},
    {10, "r10", 32, RegisterKind::General},
    {11, "r11", 32, RegisterKind::General},
    {12, "r12", 32, RegisterKind::General},
    {13, "sp", 32, RegisterKind::StackPointer},
    {14, "lr", 32, RegisterKind::Link},
    {15, "pc", 32, RegisterKind::ProgramCounter},
    {16, "cpsr", 32, RegisterKind::Flags},
    {17, "fpscr", 32, RegisterKind::FloatingPointControl},
    {32, "d0", 64, RegisterKind::Vector},
    {33, "d1", 64, RegisterKind::Vector},
    {34, "d2", 64, RegisterKind::Vector},
    {35, "d3", 64, RegisterKind::Vector},
    {36, "d4", 64, RegisterKind::Vector},
    {37, "d5", 64, RegisterKind::Vector},
    {38, "d6", 64, RegisterKind::Vector},
    {39, "d7", 64, RegisterKind::Vector},
    {40, "d8", 64, RegisterKind::Vector},
    {41, "d9", 64, RegisterKind::Vector},
    {42, "d10", 64, RegisterKind::Vector},
    {43, "d11", 64, RegisterKind::Vector},
    {44, "d12", 64, RegisterKind::Vector},
    {45, "d13", 64, RegisterKind::Vector},
    {46, "d14", 64, RegisterKind::Vector},
    {47, "d15", 64, RegisterKind::Vector},
    {48, "d16", 64, RegisterKind::Vector},
    {49, "d17", 64, RegisterKind::Vector},
    {50, "d18", 64, RegisterKind::Vector},
    {51, "d19", 64, RegisterKind::Vector},
    {52, "d20", 64, RegisterKind::Vector},
    {53, "d21", 64, RegisterKind::Vector},
    {54, "d22", 64, RegisterKind::Vector},
    {55, "d23", 64, RegisterKind::Vector},
    {56, "d24", 64, RegisterKind::Vector},
    {57, "d25", 64, RegisterKind::Vector},
    {58, "d26", 64, RegisterKind::Vector},
    {59, "d27", 64, RegisterKind::Vector},
    {60, "d28", 64, RegisterKind::Vector},
    {61, "d29", 64, RegisterKind::Vector},
    {62, "d30", 64, RegisterKind::Vector},
    {63, "d31", 64, RegisterKind::Vector},
};

constexpr GuestExecutionMode DefaultExecutionModes[] = {
    GuestExecutionMode::Default};
constexpr GuestExecutionMode ARM32ExecutionModes[] = {
    GuestExecutionMode::ARM, GuestExecutionMode::Thumb};

constexpr ArchitectureDescription GuestStateWireV1Architectures[] = {
    {GuestArchitecture::X86_64, "x86_64", GuestEndianness::Little, 64,
     GuestExecutionMode::Default, 16, 4, DefaultExecutionModes,
     GuestStateWireV1X86_64Registers},
    {GuestArchitecture::AArch64, "aarch64", GuestEndianness::Little, 64,
     GuestExecutionMode::Default, 32, 31, DefaultExecutionModes,
     GuestStateWireV1AArch64Registers},
    {GuestArchitecture::ARM32, "arm32", GuestEndianness::Little, 32,
     GuestExecutionMode::ARM, 15, 13, ARM32ExecutionModes,
     GuestStateWireV1ARM32Registers},
    {GuestArchitecture::X86_32, "x86", GuestEndianness::Little, 32,
     GuestExecutionMode::Default, 8, 4, DefaultExecutionModes,
     GuestStateWireV1X86_32Registers},
};

llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, "%s",
                                 Message.str().c_str());
}

bool isValidExtensionName(llvm::StringRef Name) {
  if (Name.empty())
    return false;
  return llvm::all_of(Name, [](char Character) {
    const unsigned char Byte = static_cast<unsigned char>(Character);
    const bool IsASCIIAlnum =
        (Byte >= 'a' && Byte <= 'z') || (Byte >= '0' && Byte <= '9');
    return IsASCIIAlnum || Character == '_' || Character == '.' ||
           Character == ':' || Character == '-';
  });
}

bool isValidFeatureName(llvm::StringRef Name) {
  if (Name.empty())
    return false;
  return llvm::all_of(Name, [](char Character) {
    const unsigned char Byte = static_cast<unsigned char>(Character);
    return (Byte >= 'a' && Byte <= 'z') || (Byte >= '0' && Byte <= '9') ||
           Character == '_' || Character == '.' || Character == '-';
  });
}

uint64_t maximumAddress(uint16_t AddressWidth) {
  if (AddressWidth == 64)
    return std::numeric_limits<uint64_t>::max();
  return (uint64_t{1} << AddressWidth) - 1;
}

llvm::Error validateRegisterRecord(const ArchitectureDescription &Description,
                                   const GuestRegisterValue &Register) {
  if (const RegisterDescription *Core =
          findRegister(Description, Register.ID)) {
    if (!Register.ExtensionName.empty())
      return invalid("architecture register carries an extension name");
    if (Register.Value.getBitWidth() != Core->BitWidth)
      return invalid(llvm::Twine("register '") + Core->Name +
                     "' has the wrong bit width");
    return llvm::Error::success();
  }

  if (Register.ID < kFirstExtensionRegisterID)
    return invalid("unknown architecture register ID");
  if (Register.ExtensionName.empty())
    return invalid("extension register name is empty");
  if (Register.Value.getBitWidth() == 0)
    return invalid("extension register bit width is zero");
  if (!isValidExtensionName(Register.ExtensionName))
    return invalid("extension register name is not canonical lower-case ASCII");
  if (findRegister(Description, Register.ExtensionName))
    return invalid("extension register name aliases an architecture register");
  return llvm::Error::success();
}

constexpr std::array<uint8_t, 8> WireMagic = {'N', 'V', 'D', 'S',
                                              'T', 'A', 'T', 'E'};
constexpr uint32_t WireHeaderSize = 64;

class WireWriter {
public:
  template <typename T> void integer(T Value) {
    static_assert(std::is_unsigned_v<T>);
    for (unsigned Byte = 0; Byte != sizeof(T); ++Byte)
      Bytes.push_back(static_cast<uint8_t>(Value >> (Byte * 8)));
  }

  void bytes(llvm::ArrayRef<uint8_t> Value) {
    Bytes.insert(Bytes.end(), Value.begin(), Value.end());
  }

  std::vector<uint8_t> take() { return std::move(Bytes); }

private:
  std::vector<uint8_t> Bytes;
};

class WireReader {
public:
  explicit WireReader(llvm::ArrayRef<uint8_t> Bytes) : Bytes(Bytes) {}

  template <typename T> llvm::Expected<T> integer(llvm::StringRef Field) {
    static_assert(std::is_unsigned_v<T>);
    if (Bytes.size() - Offset < sizeof(T))
      return invalid(llvm::Twine("truncated guest-state ") + Field);
    T Value = 0;
    for (unsigned Byte = 0; Byte != sizeof(T); ++Byte)
      Value |= static_cast<T>(Bytes[Offset++]) << (Byte * 8);
    return Value;
  }

  llvm::Expected<llvm::ArrayRef<uint8_t>> bytes(uint64_t Size,
                                                llvm::StringRef Field) {
    if (Size > Bytes.size() - Offset)
      return invalid(llvm::Twine("truncated guest-state ") + Field);
    llvm::ArrayRef<uint8_t> Result = Bytes.slice(Offset, Size);
    Offset += static_cast<size_t>(Size);
    return Result;
  }

  bool empty() const { return Offset == Bytes.size(); }
  uint64_t remaining() const {
    return static_cast<uint64_t>(Bytes.size() - Offset);
  }

private:
  llvm::ArrayRef<uint8_t> Bytes;
  size_t Offset = 0;
};

template <typename T>
llvm::Expected<T> readInteger(WireReader &Reader, llvm::StringRef Field) {
  return Reader.integer<T>(Field);
}

llvm::Error requireZero(llvm::ArrayRef<uint8_t> Bytes, llvm::StringRef Field) {
  if (llvm::any_of(Bytes, [](uint8_t Byte) { return Byte != 0; }))
    return invalid(llvm::Twine("non-zero reserved guest-state ") + Field);
  return llvm::Error::success();
}

void writeAPInt(WireWriter &Writer, const llvm::APInt &Value) {
  const uint64_t Width = Value.getBitWidth();
  const uint64_t ByteCount = (Width + 7) / 8;
  for (uint64_t Byte = 0; Byte != ByteCount; ++Byte) {
    const uint64_t Offset = Byte * 8;
    const unsigned Bits =
        static_cast<unsigned>(std::min<uint64_t>(8, Width - Offset));
    Writer.integer<uint8_t>(static_cast<uint8_t>(
        Value.extractBitsAsZExtValue(Bits, static_cast<unsigned>(Offset))));
  }
}

bool exceedsLimit(uint64_t Value, uint64_t Limit) {
  return Limit != 0 && Value > Limit;
}

llvm::Expected<llvm::APInt> readAPInt(uint32_t Width,
                                      llvm::ArrayRef<uint8_t> Bytes) {
  if (Width == 0)
    return invalid("register bit width is zero");
  const uint64_t ExpectedSize = (uint64_t{Width} + 7) / 8;
  if (Bytes.size() != ExpectedSize)
    return invalid("register value length does not match its bit width");
  const unsigned Remainder = Width % 8;
  if (Remainder != 0 &&
      (Bytes.back() & static_cast<uint8_t>(~((1u << Remainder) - 1))) != 0)
    return invalid("register value has non-zero wire padding bits");

  llvm::SmallVector<uint64_t, 4> Words((uint64_t{Width} + 63) / 64, 0);
  for (size_t Index = 0; Index < Bytes.size(); ++Index)
    Words[Index / 8] |= uint64_t{Bytes[Index]} << ((Index % 8) * 8);
  return llvm::APInt(Width, Words);
}

} // namespace

const ArchitectureDescription *
getArchitectureDescription(GuestArchitecture Architecture) {
  const auto It = llvm::find_if(
      GuestStateWireV1Architectures, [&](const auto &Description) {
        return Description.Architecture == Architecture;
      });
  return It == std::end(GuestStateWireV1Architectures) ? nullptr : It;
}

const RegisterDescription *findRegister(const ArchitectureDescription &Arch,
                                        RegisterID ID) {
  const auto It =
      llvm::find_if(Arch.Registers, [&](const RegisterDescription &Register) {
        return Register.ID == ID;
      });
  return It == Arch.Registers.end() ? nullptr : &*It;
}

const RegisterDescription *findRegister(const ArchitectureDescription &Arch,
                                        llvm::StringRef Name) {
  const auto It = llvm::find_if(Arch.Registers, [&](const auto &Register) {
    return Register.Name.equals_insensitive(Name);
  });
  return It == Arch.Registers.end() ? nullptr : &*It;
}

llvm::Expected<GuestState>
createZeroedGuestState(GuestArchitecture Architecture, uint64_t ThreadID) {
  const ArchitectureDescription *Description =
      getArchitectureDescription(Architecture);
  if (!Description)
    return invalid("unknown guest architecture");

  GuestState State;
  State.Architecture = Architecture;
  State.ByteOrder = Description->ByteOrder;
  State.ExecutionMode = Description->DefaultMode;
  State.AddressWidth = Description->AddressWidth;
  State.ThreadID = ThreadID;
  State.Registers.reserve(Description->Registers.size());
  for (const RegisterDescription &Register : Description->Registers)
    State.Registers.push_back(
        {Register.ID, {}, llvm::APInt(Register.BitWidth, 0)});
  return State;
}

const GuestRegisterValue *findRegisterValue(const GuestState &State,
                                            RegisterID ID) {
  const auto It =
      llvm::find_if(State.Registers, [ID](const GuestRegisterValue &Register) {
        return Register.ID == ID;
      });
  return It == State.Registers.end() ? nullptr : &*It;
}

GuestRegisterValue *findRegisterValue(GuestState &State, RegisterID ID) {
  const auto It =
      llvm::find_if(State.Registers, [ID](const GuestRegisterValue &Register) {
        return Register.ID == ID;
      });
  return It == State.Registers.end() ? nullptr : &*It;
}

llvm::Error setRegisterValue(GuestState &State, RegisterID ID,
                             const llvm::APInt &Value,
                             llvm::StringRef ExtensionName) {
  const ArchitectureDescription *Description =
      getArchitectureDescription(State.Architecture);
  if (!Description)
    return invalid("unknown guest architecture");

  GuestRegisterValue Candidate{ID, ExtensionName.str(), Value};
  if (llvm::Error Error = validateRegisterRecord(*Description, Candidate))
    return Error;

  if (ID >= kFirstExtensionRegisterID) {
    const bool NameCollision =
        llvm::any_of(State.Registers, [&](const GuestRegisterValue &Register) {
          return Register.ID != ID &&
                 Register.ID >= kFirstExtensionRegisterID &&
                 llvm::StringRef(Register.ExtensionName)
                     .equals_insensitive(ExtensionName);
        });
    if (NameCollision)
      return invalid("extension register name is not unique");
  }

  if (GuestRegisterValue *Existing = findRegisterValue(State, ID))
    *Existing = std::move(Candidate);
  else
    State.Registers.push_back(std::move(Candidate));
  return llvm::Error::success();
}

llvm::Error validateGuestState(const GuestState &State) {
  const ArchitectureDescription *Description =
      getArchitectureDescription(State.Architecture);
  if (!Description)
    return invalid("unknown guest architecture");
  if (State.ByteOrder != Description->ByteOrder)
    return invalid("guest byte order does not match its architecture");
  if (!llvm::is_contained(Description->ExecutionModes, State.ExecutionMode))
    return invalid("guest execution mode is invalid for its architecture");
  if (State.AddressWidth != Description->AddressWidth)
    return invalid("guest address width does not match its architecture");

  std::set<std::string> FeatureNames;
  for (const std::string &Feature : State.Features) {
    if (!isValidFeatureName(Feature))
      return invalid(
          "guest ISA feature name is not canonical lower-case ASCII");
    if (!FeatureNames.insert(Feature).second)
      return invalid("guest state contains a duplicate ISA feature");
  }

  llvm::DenseSet<RegisterID> IDs;
  std::set<std::string> ExtensionNames;
  for (const GuestRegisterValue &Register : State.Registers) {
    if (!IDs.insert(Register.ID).second)
      return invalid("guest state contains a duplicate register ID");
    if (llvm::Error Error = validateRegisterRecord(*Description, Register))
      return Error;
    if (Register.ID >= kFirstExtensionRegisterID) {
      std::string Folded = llvm::StringRef(Register.ExtensionName).lower();
      if (!ExtensionNames.insert(std::move(Folded)).second)
        return invalid("extension register name is not unique");
    }
  }
  for (const RegisterDescription &Register : Description->Registers) {
    if (!IDs.contains(Register.ID))
      return invalid(llvm::Twine("guest state is missing register '") +
                     Register.Name + "'");
  }

  if (State.Architecture == GuestArchitecture::ARM32) {
    const GuestRegisterValue *PC =
        findRegisterValue(State, Description->ProgramCounter);
    const GuestRegisterValue *CPSR = findRegisterValue(State, 16);
    if (!PC || !CPSR)
      return invalid("ARM32 guest state is missing PC or CPSR");
    const bool IsThumb = State.ExecutionMode == GuestExecutionMode::Thumb;
    if (CPSR->Value[5] != IsThumb)
      return invalid("ARM32 CPSR.T does not match the execution mode");
    const uint64_t ProgramCounter = PC->Value.getZExtValue();
    if ((ProgramCounter & 1u) != 0)
      return invalid("ARM32 PC is not a canonical instruction address");
    if (!IsThumb && (ProgramCounter & 3u) != 0)
      return invalid("ARM32 ARM-mode PC is not word aligned");
  }

  const uint64_t MaxAddress = maximumAddress(State.AddressWidth);
  struct Range {
    uint64_t Begin;
    uint64_t End;
  };
  std::vector<Range> Ranges;
  Ranges.reserve(State.Memory.size());
  for (const GuestMemoryRegion &Region : State.Memory) {
    const uint8_t Permissions = static_cast<uint8_t>(Region.Permissions);
    if ((Permissions & ~uint8_t{0x7}) != 0)
      return invalid("memory region has unknown permission bits");
    if (Region.Permissions == MemoryPermission::None)
      return invalid("memory region has no permissions");
    if (Region.Bytes.empty())
      return invalid("memory region is empty");
    if (Region.Address > MaxAddress ||
        Region.Bytes.size() - 1 > MaxAddress - Region.Address)
      return invalid("memory region exceeds the guest address space");
    Ranges.push_back(
        {Region.Address,
         Region.Address + static_cast<uint64_t>(Region.Bytes.size() - 1)});
  }
  llvm::sort(Ranges, [](const Range &Left, const Range &Right) {
    return Left.Begin < Right.Begin;
  });
  for (size_t Index = 1; Index < Ranges.size(); ++Index) {
    if (Ranges[Index].Begin <= Ranges[Index - 1].End)
      return invalid("guest memory regions overlap");
  }

  if (State.Exception) {
    switch (State.Exception->Kind) {
    case GuestExceptionKind::HardwareFault:
    case GuestExceptionKind::Software:
    case GuestExceptionKind::Signal:
      break;
    default:
      return invalid("unknown guest exception kind");
    }
    if (State.Exception->FaultAddress > MaxAddress ||
        State.Exception->InstructionAddress > MaxAddress)
      return invalid("guest exception address exceeds the address space");
  }
  return llvm::Error::success();
}

llvm::Expected<std::vector<uint8_t>>
serializeGuestState(const GuestState &State) {
  if (llvm::Error Error = validateGuestState(State))
    return std::move(Error);

  std::vector<const GuestRegisterValue *> Registers;
  Registers.reserve(State.Registers.size());
  for (const GuestRegisterValue &Register : State.Registers)
    Registers.push_back(&Register);
  llvm::sort(Registers, [](const auto *Left, const auto *Right) {
    return Left->ID < Right->ID;
  });

  std::vector<llvm::StringRef> Features;
  Features.reserve(State.Features.size());
  for (const std::string &Feature : State.Features)
    Features.push_back(Feature);
  llvm::sort(Features);

  std::vector<const GuestMemoryRegion *> Memory;
  Memory.reserve(State.Memory.size());
  for (const GuestMemoryRegion &Region : State.Memory)
    Memory.push_back(&Region);
  llvm::sort(Memory, [](const auto *Left, const auto *Right) {
    return Left->Address < Right->Address;
  });

  WireWriter Writer;
  Writer.bytes(WireMagic);
  Writer.integer<uint32_t>(kGuestStateWireVersion);
  Writer.integer<uint32_t>(WireHeaderSize);
  Writer.integer<uint8_t>(static_cast<uint8_t>(State.Architecture));
  Writer.integer<uint8_t>(static_cast<uint8_t>(State.ByteOrder));
  Writer.integer<uint8_t>(static_cast<uint8_t>(State.ExecutionMode));
  Writer.integer<uint8_t>(State.Exception.has_value() ? 1 : 0);
  Writer.integer<uint16_t>(State.AddressWidth);
  Writer.bytes(std::array<uint8_t, 2>{});
  Writer.integer<uint64_t>(State.ThreadID);
  Writer.integer<uint64_t>(Features.size());
  Writer.integer<uint64_t>(Registers.size());
  Writer.integer<uint64_t>(Memory.size());
  Writer.bytes(std::array<uint8_t, 8>{});

  for (llvm::StringRef Feature : Features) {
    Writer.integer<uint64_t>(Feature.size());
    Writer.bytes(llvm::ArrayRef<uint8_t>(
        reinterpret_cast<const uint8_t *>(Feature.data()), Feature.size()));
  }

  for (const GuestRegisterValue *Register : Registers) {
    Writer.integer<uint32_t>(Register->ID);
    Writer.integer<uint32_t>(Register->Value.getBitWidth());
    Writer.integer<uint64_t>(Register->ExtensionName.size());
    Writer.integer<uint64_t>((uint64_t{Register->Value.getBitWidth()} + 7) / 8);
    Writer.bytes(llvm::ArrayRef<uint8_t>(
        reinterpret_cast<const uint8_t *>(Register->ExtensionName.data()),
        Register->ExtensionName.size()));
    writeAPInt(Writer, Register->Value);
  }

  for (const GuestMemoryRegion *Region : Memory) {
    Writer.integer<uint64_t>(Region->Address);
    Writer.integer<uint64_t>(Region->Generation);
    Writer.integer<uint64_t>(Region->Bytes.size());
    Writer.integer<uint8_t>(static_cast<uint8_t>(Region->Permissions));
    Writer.bytes(std::array<uint8_t, 7>{});
    Writer.bytes(Region->Bytes);
  }

  if (State.Exception) {
    Writer.integer<uint8_t>(static_cast<uint8_t>(State.Exception->Kind));
    Writer.bytes(std::array<uint8_t, 7>{});
    Writer.integer<uint64_t>(State.Exception->Code);
    Writer.integer<uint64_t>(State.Exception->FaultAddress);
    Writer.integer<uint64_t>(State.Exception->InstructionAddress);
    Writer.integer<uint64_t>(State.Exception->Payload.size());
    Writer.bytes(State.Exception->Payload);
  }
  return Writer.take();
}

llvm::Expected<GuestState>
deserializeGuestState(llvm::ArrayRef<uint8_t> Bytes,
                      const GuestStateWireLimits &Limits) {
  if (exceedsLimit(Bytes.size(), Limits.MaxWireBytes))
    return invalid("guest-state wire exceeds the caller's byte budget");
  WireReader Reader(Bytes);
  llvm::Expected<llvm::ArrayRef<uint8_t>> Magic =
      Reader.bytes(WireMagic.size(), "magic");
  if (!Magic)
    return Magic.takeError();
  if (!std::equal(Magic->begin(), Magic->end(), WireMagic.begin()))
    return invalid("invalid guest-state wire magic");

  llvm::Expected<uint32_t> Version = readInteger<uint32_t>(Reader, "version");
  if (!Version)
    return Version.takeError();
  if (*Version != kGuestStateWireVersion)
    return invalid(llvm::Twine("unsupported guest-state wire version ") +
                   llvm::Twine(*Version));

  llvm::Expected<uint32_t> HeaderSize =
      readInteger<uint32_t>(Reader, "header size");
  if (!HeaderSize)
    return HeaderSize.takeError();
  if (*HeaderSize != WireHeaderSize)
    return invalid("unsupported guest-state wire header size");

  llvm::Expected<uint8_t> Architecture =
      readInteger<uint8_t>(Reader, "architecture");
  if (!Architecture)
    return Architecture.takeError();
  llvm::Expected<uint8_t> ByteOrder =
      readInteger<uint8_t>(Reader, "byte order");
  if (!ByteOrder)
    return ByteOrder.takeError();
  llvm::Expected<uint8_t> ExecutionMode =
      readInteger<uint8_t>(Reader, "execution mode");
  if (!ExecutionMode)
    return ExecutionMode.takeError();
  llvm::Expected<uint8_t> HasException =
      readInteger<uint8_t>(Reader, "exception marker");
  if (!HasException)
    return HasException.takeError();
  llvm::Expected<uint16_t> AddressWidth =
      readInteger<uint16_t>(Reader, "address width");
  if (!AddressWidth)
    return AddressWidth.takeError();
  llvm::Expected<llvm::ArrayRef<uint8_t>> AddressReserved =
      Reader.bytes(2, "address reserved bytes");
  if (!AddressReserved)
    return AddressReserved.takeError();
  if (llvm::Error Error = requireZero(*AddressReserved, "address bytes"))
    return std::move(Error);
  llvm::Expected<uint64_t> ThreadID =
      readInteger<uint64_t>(Reader, "thread ID");
  if (!ThreadID)
    return ThreadID.takeError();
  llvm::Expected<uint64_t> FeatureCount =
      readInteger<uint64_t>(Reader, "feature count");
  if (!FeatureCount)
    return FeatureCount.takeError();
  llvm::Expected<uint64_t> RegisterCount =
      readInteger<uint64_t>(Reader, "register count");
  if (!RegisterCount)
    return RegisterCount.takeError();
  llvm::Expected<uint64_t> MemoryCount =
      readInteger<uint64_t>(Reader, "memory-region count");
  if (!MemoryCount)
    return MemoryCount.takeError();
  llvm::Expected<llvm::ArrayRef<uint8_t>> HeaderReserved =
      Reader.bytes(8, "header reserved bytes");
  if (!HeaderReserved)
    return HeaderReserved.takeError();
  if (llvm::Error Error = requireZero(*HeaderReserved, "header bytes"))
    return std::move(Error);
  if (*HasException > 1)
    return invalid("invalid guest-state exception marker");
  if (exceedsLimit(*FeatureCount, Limits.MaxFeatures))
    return invalid("guest-state feature count exceeds the caller's budget");
  if (exceedsLimit(*RegisterCount, Limits.MaxRegisters))
    return invalid("guest-state register count exceeds the caller's budget");
  if (exceedsLimit(*MemoryCount, Limits.MaxMemoryRegions))
    return invalid(
        "guest-state memory-region count exceeds the caller's budget");
  if (*FeatureCount > Reader.remaining() / sizeof(uint64_t))
    return invalid("guest-state feature count exceeds the remaining wire");

  GuestState State;
  State.Architecture = static_cast<GuestArchitecture>(*Architecture);
  State.ByteOrder = static_cast<GuestEndianness>(*ByteOrder);
  State.ExecutionMode = static_cast<GuestExecutionMode>(*ExecutionMode);
  State.AddressWidth = *AddressWidth;
  State.ThreadID = *ThreadID;

  const ArchitectureDescription *Description =
      getArchitectureDescription(State.Architecture);
  if (!Description)
    return invalid("unknown guest architecture");
  if (State.ByteOrder != Description->ByteOrder)
    return invalid("guest byte order does not match its architecture");
  if (!llvm::is_contained(Description->ExecutionModes, State.ExecutionMode))
    return invalid("guest execution mode is invalid for its architecture");
  if (State.AddressWidth != Description->AddressWidth)
    return invalid("guest address width does not match its architecture");

  std::optional<std::string> PreviousFeature;
  for (uint64_t Index = 0; Index < *FeatureCount; ++Index) {
    llvm::Expected<uint64_t> Size =
        readInteger<uint64_t>(Reader, "feature name length");
    if (!Size)
      return Size.takeError();
    if (*Size == 0)
      return invalid("guest ISA feature name is empty");
    llvm::Expected<llvm::ArrayRef<uint8_t>> Name =
        Reader.bytes(*Size, "feature name");
    if (!Name)
      return Name.takeError();
    std::string Feature(reinterpret_cast<const char *>(Name->data()),
                        Name->size());
    if (PreviousFeature && Feature <= *PreviousFeature)
      return invalid("guest ISA feature records are not canonical");
    PreviousFeature = Feature;
    State.Features.push_back(std::move(Feature));
  }

  std::optional<RegisterID> PreviousRegister;
  if (*RegisterCount > Reader.remaining() / 24)
    return invalid("guest-state register count exceeds the remaining wire");
  for (uint64_t Index = 0; Index < *RegisterCount; ++Index) {
    llvm::Expected<uint32_t> ID = readInteger<uint32_t>(Reader, "register ID");
    if (!ID)
      return ID.takeError();
    if (PreviousRegister && *ID <= *PreviousRegister)
      return invalid("guest register records are not canonical");
    PreviousRegister = *ID;
    llvm::Expected<uint32_t> Width =
        readInteger<uint32_t>(Reader, "register bit width");
    if (!Width)
      return Width.takeError();
    llvm::Expected<uint64_t> NameSize =
        readInteger<uint64_t>(Reader, "register name length");
    if (!NameSize)
      return NameSize.takeError();
    llvm::Expected<uint64_t> ValueSize =
        readInteger<uint64_t>(Reader, "register value length");
    if (!ValueSize)
      return ValueSize.takeError();
    if (exceedsLimit(*Width, Limits.MaxRegisterBits))
      return invalid("guest-state register width exceeds the caller's budget");
    llvm::Expected<llvm::ArrayRef<uint8_t>> Name =
        Reader.bytes(*NameSize, "register name");
    if (!Name)
      return Name.takeError();
    llvm::Expected<llvm::ArrayRef<uint8_t>> Value =
        Reader.bytes(*ValueSize, "register value");
    if (!Value)
      return Value.takeError();
    llvm::Expected<llvm::APInt> Integer = readAPInt(*Width, *Value);
    if (!Integer)
      return Integer.takeError();
    std::string ExtensionName;
    if (!Name->empty())
      ExtensionName.assign(reinterpret_cast<const char *>(Name->data()),
                           Name->size());
    State.Registers.push_back(
        {*ID, std::move(ExtensionName), std::move(*Integer)});
  }

  std::optional<uint64_t> PreviousMemoryAddress;
  uint64_t TotalMemoryBytes = 0;
  if (*MemoryCount > Reader.remaining() / 32)
    return invalid(
        "guest-state memory-region count exceeds the remaining wire");
  for (uint64_t Index = 0; Index < *MemoryCount; ++Index) {
    llvm::Expected<uint64_t> Address =
        readInteger<uint64_t>(Reader, "memory-region address");
    if (!Address)
      return Address.takeError();
    if (PreviousMemoryAddress && *Address <= *PreviousMemoryAddress)
      return invalid("guest memory records are not canonical");
    PreviousMemoryAddress = *Address;
    llvm::Expected<uint64_t> Generation =
        readInteger<uint64_t>(Reader, "memory-region generation");
    if (!Generation)
      return Generation.takeError();
    llvm::Expected<uint64_t> Size =
        readInteger<uint64_t>(Reader, "memory-region length");
    if (!Size)
      return Size.takeError();
    llvm::Expected<uint8_t> Permissions =
        readInteger<uint8_t>(Reader, "memory-region permissions");
    if (!Permissions)
      return Permissions.takeError();
    llvm::Expected<llvm::ArrayRef<uint8_t>> Reserved =
        Reader.bytes(7, "memory-region reserved bytes");
    if (!Reserved)
      return Reserved.takeError();
    if (llvm::Error Error = requireZero(*Reserved, "memory-region bytes"))
      return std::move(Error);
    if (exceedsLimit(*Size, Limits.MaxGuestMemoryBytes) ||
        TotalMemoryBytes > std::numeric_limits<uint64_t>::max() - *Size ||
        exceedsLimit(TotalMemoryBytes + *Size, Limits.MaxGuestMemoryBytes))
      return invalid("guest-state memory bytes exceed the caller's budget");
    TotalMemoryBytes += *Size;
    llvm::Expected<llvm::ArrayRef<uint8_t>> Contents =
        Reader.bytes(*Size, "memory-region contents");
    if (!Contents)
      return Contents.takeError();
    State.Memory.push_back(
        {*Address, static_cast<MemoryPermission>(*Permissions), *Generation,
         std::vector<uint8_t>(Contents->begin(), Contents->end())});
  }

  if (*HasException != 0) {
    llvm::Expected<uint8_t> Kind =
        readInteger<uint8_t>(Reader, "exception kind");
    if (!Kind)
      return Kind.takeError();
    llvm::Expected<llvm::ArrayRef<uint8_t>> Reserved =
        Reader.bytes(7, "exception reserved bytes");
    if (!Reserved)
      return Reserved.takeError();
    if (llvm::Error Error = requireZero(*Reserved, "exception bytes"))
      return std::move(Error);
    llvm::Expected<uint64_t> Code =
        readInteger<uint64_t>(Reader, "exception code");
    if (!Code)
      return Code.takeError();
    llvm::Expected<uint64_t> FaultAddress =
        readInteger<uint64_t>(Reader, "exception fault address");
    if (!FaultAddress)
      return FaultAddress.takeError();
    llvm::Expected<uint64_t> InstructionAddress =
        readInteger<uint64_t>(Reader, "exception instruction address");
    if (!InstructionAddress)
      return InstructionAddress.takeError();
    llvm::Expected<uint64_t> PayloadSize =
        readInteger<uint64_t>(Reader, "exception payload length");
    if (!PayloadSize)
      return PayloadSize.takeError();
    if (exceedsLimit(*PayloadSize, Limits.MaxExceptionPayloadBytes))
      return invalid(
          "guest-state exception payload exceeds the caller's budget");
    llvm::Expected<llvm::ArrayRef<uint8_t>> Payload =
        Reader.bytes(*PayloadSize, "exception payload");
    if (!Payload)
      return Payload.takeError();
    State.Exception = GuestExceptionState{
        static_cast<GuestExceptionKind>(*Kind), *Code, *FaultAddress,
        *InstructionAddress,
        std::vector<uint8_t>(Payload->begin(), Payload->end())};
  }

  if (!Reader.empty())
    return invalid("guest-state wire has trailing bytes");
  if (llvm::Error Error = validateGuestState(State))
    return std::move(Error);
  return State;
}

} // namespace neverd::translate
