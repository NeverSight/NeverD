//===- KernelModelRuntime.cpp - Checked guest runtime services ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Guest counted strings and finite Windows debug formatting. The model never
/// passes a guest format, pointer, or variadic list to the host C runtime.
/// Contracts: Microsoft Learn RtlCopyUnicodeString, RtlCompareUnicodeString,
/// DbgPrint, and format-specification-syntax-printf-and-wprintf-functions.
///
//===----------------------------------------------------------------------===//

#include "KernelModelRuntime.h"

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <array>
#include <optional>
#include <tuple>
#include <vector>

namespace neverd::emulation::runtime {
namespace {
using namespace windows;
constexpr uint64_t MaxASCII = 0x7f;

// System time counts 100 ns units from 1601-01-01, a Monday, in the
// Gregorian calendar. TIME_FIELDS holds eight CSHORTs, Year through Weekday.
constexpr unsigned TimeFieldCount = 8;
constexpr uint64_t TimeUnitsPerMillisecond = 10000;
constexpr uint64_t MillisecondsPerDay = 86400000;
constexpr uint64_t DaysPer400Years = 146097;
constexpr uint64_t DaysPer100Years = 36524;
constexpr uint64_t DaysPer4Years = 1461;
constexpr uint64_t DaysPerYear = 365;
constexpr int64_t FirstTimeFieldsYear = 1601;
constexpr int64_t LastTimeFieldsYear = 30827;
constexpr uint16_t DaysPrecedingMonth[2][13] = {
    {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365},
    {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366}};

bool isLeapYear(uint64_t Year) {
  return (Year % 4 == 0 && Year % 100 != 0) || Year % 400 == 0;
}

#define NEVERD_KERNEL_RUNTIME_STRING(Name, Value)                              \
  constexpr llvm::StringLiteral Name(Value);
#include "KernelRuntimeStrings.def"
#undef NEVERD_KERNEL_RUNTIME_STRING

llvm::Error runtimeError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

llvm::Expected<uint64_t> readInteger(const KernelModel &Model,
                                     GuestMemory &Memory, uint64_t Address,
                                     unsigned Size) {
  if (auto E = Model.validateGuestAccess(Address, Size, false))
    return E;
  return Memory.readInteger(Address, Size);
}

struct CountedString {
  uint16_t Length;
  uint16_t MaximumLength;
  uint64_t Buffer;
};

llvm::Expected<CountedString> readCountedString(const KernelModel &Model,
                                                GuestMemory &Memory,
                                                uint64_t Address) {
  if (!Address)
    return runtimeError("UNICODE_STRING record must not be null");
  if (auto E = Model.validateGuestAccess(Address, UnicodeRecordSize, false))
    return E;
  auto Length = Memory.readInteger(Address, 2);
  if (!Length)
    return Length.takeError();
  auto Maximum = Memory.readInteger(Address + UnicodeMaximumOffset, 2);
  if (!Maximum)
    return Maximum.takeError();
  auto Buffer = Memory.readInteger(Address + UnicodeBufferOffset, 8);
  if (!Buffer)
    return Buffer.takeError();
  if ((*Length & 1) || (*Maximum & 1) || *Length > *Maximum ||
      *Maximum > MaxCountedUnicodeBytes || (*Length && !*Buffer))
    return runtimeError("invalid counted UTF-16 string lengths or buffer");
  return CountedString{static_cast<uint16_t>(*Length),
                       static_cast<uint16_t>(*Maximum), *Buffer};
}

// STRING shares the UNICODE_STRING layout but counts bytes.
llvm::Expected<CountedString> readCountedBytes(const KernelModel &Model,
                                               GuestMemory &Memory,
                                               uint64_t Address) {
  if (!Address)
    return runtimeError("STRING record must not be null");
  if (auto E = Model.validateGuestAccess(Address, UnicodeRecordSize, false))
    return E;
  auto Length = Memory.readInteger(Address, 2);
  if (!Length)
    return Length.takeError();
  auto Maximum = Memory.readInteger(Address + UnicodeMaximumOffset, 2);
  if (!Maximum)
    return Maximum.takeError();
  auto Buffer = Memory.readInteger(Address + UnicodeBufferOffset, 8);
  if (!Buffer)
    return Buffer.takeError();
  if (*Length > *Maximum || (*Length && !*Buffer))
    return runtimeError("invalid counted 8-bit string length or buffer");
  return CountedString{static_cast<uint16_t>(*Length),
                       static_cast<uint16_t>(*Maximum), *Buffer};
}

llvm::Expected<std::vector<uint8_t>> readBytes(const KernelModel &Model,
                                               GuestMemory &Memory,
                                               uint64_t Address,
                                               unsigned Size) {
  if (auto E = Model.validateGuestAccess(Address, Size, false))
    return E;
  std::vector<uint8_t> Bytes(Size);
  if (Size)
    if (auto E = Memory.read(Address, Bytes))
      return E;
  return Bytes;
}

uint16_t codeUnit(llvm::ArrayRef<uint8_t> Bytes, size_t Index) {
  return Bytes[Index] | (uint16_t(Bytes[Index + 1]) << 8);
}

enum class Conversion {
  SignedDecimal,
  UnsignedDecimal,
  Octal,
  HexLower,
  HexUpper,
  Pointer,
  String,
  Character,
  CountedString
};

std::optional<Conversion> conversion(char Code) {
  switch (Code) {
#define NEVERD_KERNEL_DEBUG_FORMAT(Code, Kind)                                 \
  case Code:                                                                   \
    return Conversion::Kind;
#include "KernelDebugFormats.def"
#undef NEVERD_KERNEL_DEBUG_FORMAT
  default:
    return std::nullopt;
  }
}

enum class LengthKind {
  None,
#define NEVERD_KERNEL_DEBUG_LENGTH(Spelling, Name, Bits) Name,
#include "KernelDebugLengths.def"
#undef NEVERD_KERNEL_DEBUG_LENGTH
};

struct FormatSpec {
  bool Left = false;
  bool Plus = false;
  bool Space = false;
  bool Zero = false;
  bool Alternate = false;
  unsigned Width = 0;
  std::optional<unsigned> Precision;
  LengthKind Length = LengthKind::None;
  unsigned Bits = 32;
};

class Formatter {
  const KernelModel &Model;
  GuestMemory &Memory;
  using TypedArgumentReader =
      llvm::function_ref<llvm::Expected<uint64_t>(unsigned, unsigned)>;
  TypedArgumentReader ReadArgument;
  unsigned NextArgument;
  unsigned UsedArguments = 0;
  std::string Output;

  llvm::Expected<uint64_t> argument(unsigned Size) {
    if (UsedArguments == MaxDebugArguments)
      return runtimeError("DbgPrint exceeds the variadic argument limit");
    if (!ReadArgument)
      return runtimeError("DbgPrint variadic argument reader is unavailable");
    ++UsedArguments;
    return ReadArgument(NextArgument++, Size);
  }

  llvm::Error append(llvm::StringRef Text) {
    if (Text.size() > MaxDebugBytes - Output.size())
      return runtimeError(llvm::formatv(
          "DbgPrint output exceeds the {0}-byte model limit", MaxDebugBytes));
    Output.append(Text.data(), Text.size());
    return llvm::Error::success();
  }

  llvm::Expected<unsigned> number(llvm::StringRef Format, size_t &Position) {
    unsigned Value = 0;
    while (Position < Format.size() && Format[Position] >= '0' &&
           Format[Position] <= '9') {
      unsigned Digit = Format[Position++] - '0';
      if (Value > (MaxDebugFieldWidth - Digit) / 10)
        return runtimeError("DbgPrint width or precision exceeds model limit");
      Value = Value * 10 + Digit;
    }
    return Value;
  }

  llvm::Expected<FormatSpec> spec(llvm::StringRef Format, size_t &Position) {
    FormatSpec Spec;
    while (Position < Format.size()) {
      bool Flag = true;
      switch (Format[Position]) {
      case '-':
        Spec.Left = true;
        break;
      case '+':
        Spec.Plus = true;
        break;
      case ' ':
        Spec.Space = true;
        break;
      case '0':
        Spec.Zero = true;
        break;
      case '#':
        Spec.Alternate = true;
        break;
      default:
        Flag = false;
        break;
      }
      if (!Flag)
        break;
      ++Position;
    }
    if (Position < Format.size() && Format[Position] == '*') {
      ++Position;
      auto Width = argument(4);
      if (!Width)
        return Width.takeError();
      const int64_t SignedWidth = static_cast<int32_t>(*Width);
      Spec.Left |= SignedWidth < 0;
      const uint64_t Magnitude = SignedWidth < 0 ? -SignedWidth : SignedWidth;
      if (Magnitude > MaxDebugFieldWidth)
        return runtimeError("DbgPrint width exceeds model limit");
      Spec.Width = Magnitude;
    } else {
      auto Width = number(Format, Position);
      if (!Width)
        return Width.takeError();
      Spec.Width = *Width;
    }
    if (Position < Format.size() && Format[Position] == '.') {
      ++Position;
      if (Position < Format.size() && Format[Position] == '*') {
        ++Position;
        auto Precision = argument(4);
        if (!Precision)
          return Precision.takeError();
        const int32_t SignedPrecision = static_cast<int32_t>(*Precision);
        if (SignedPrecision > static_cast<int32_t>(MaxDebugFieldWidth))
          return runtimeError("DbgPrint precision exceeds model limit");
        if (SignedPrecision >= 0)
          Spec.Precision = SignedPrecision;
      } else {
        auto Precision = number(Format, Position);
        if (!Precision)
          return Precision.takeError();
        Spec.Precision = *Precision;
      }
    }
    struct LengthDescription {
      llvm::StringLiteral Spelling;
      LengthKind Kind;
      unsigned Bits;
    };
    static constexpr LengthDescription Lengths[] = {
#define NEVERD_KERNEL_DEBUG_LENGTH(Spelling, Name, Bits)                       \
  {Spelling, LengthKind::Name, Bits},
#include "KernelDebugLengths.def"
#undef NEVERD_KERNEL_DEBUG_LENGTH
    };
    for (const auto &Length : Lengths)
      if (Format.drop_front(Position).starts_with(Length.Spelling)) {
        Position += Length.Spelling.size();
        Spec.Length = Length.Kind;
        Spec.Bits = Length.Bits;
        break;
      }
    return Spec;
  }

  llvm::Expected<std::string> string(uint64_t Address, bool Wide,
                                     std::optional<unsigned> Precision) {
    const unsigned Limit = Precision.value_or(MaxDebugBytes);
    if (!Address)
      return NullString.take_front(Limit).str();
    std::string Text;
    const unsigned Step = Wide ? 2 : 1;
    for (unsigned I = 0; I < Limit; ++I) {
      if (Address > UINT64_MAX - uint64_t(I) * Step)
        return runtimeError("DbgPrint string address overflows");
      auto Unit =
          readInteger(Model, Memory, Address + uint64_t(I) * Step, Step);
      if (!Unit)
        return Unit.takeError();
      if (!*Unit)
        return Text;
      if (*Unit > MaxASCII)
        return runtimeError(
            "DbgPrint non-ASCII text requires an unmodeled code page");
      Text.push_back(static_cast<char>(*Unit));
    }
    if (Precision)
      return Text;
    if (Address > UINT64_MAX - uint64_t(Limit) * Step)
      return runtimeError("DbgPrint string address overflows");
    auto Terminator =
        readInteger(Model, Memory, Address + uint64_t(Limit) * Step, Step);
    if (!Terminator)
      return Terminator.takeError();
    if (*Terminator)
      return runtimeError("unterminated DbgPrint string within model limit");
    return Text;
  }

  llvm::Expected<std::string> countedString(uint64_t Address,
                                            const FormatSpec &Spec) {
    if (Model.currentIRQL() != scheduler::PassiveLevel)
      return runtimeError("Unicode DbgPrint requires IRQL PASSIVE_LEVEL");
    if (Spec.Length != LengthKind::Wide && Spec.Length != LengthKind::Long)
      return runtimeError("DbgPrint counted strings require %wZ or %lZ");
    auto Record = readCountedString(Model, Memory, Address);
    if (!Record)
      return Record.takeError();
    if (!Record->Buffer)
      return NullString.take_front(Spec.Precision.value_or(MaxDebugBytes))
          .str();
    unsigned Units = Record->Length / 2;
    if (Spec.Precision)
      Units = std::min(Units, *Spec.Precision);
    if (Units > MaxDebugBytes)
      return runtimeError("DbgPrint counted string exceeds model limit");
    auto Bytes = readBytes(Model, Memory, Record->Buffer, Units * 2);
    if (!Bytes)
      return Bytes.takeError();
    std::string Text;
    for (unsigned I = 0; I < Units; ++I) {
      uint16_t Unit = codeUnit(*Bytes, I * 2);
      if (Unit > MaxASCII)
        return runtimeError(
            "DbgPrint non-ASCII text requires an unmodeled code page");
      Text.push_back(static_cast<char>(Unit));
    }
    return Text;
  }

  llvm::Error render(Conversion Kind, const FormatSpec &Spec, uint64_t Value) {
    std::string Prefix;
    std::string Text;
    const bool Numeric = Kind <= Conversion::Pointer;
    if (Numeric) {
      if (Spec.Length == LengthKind::Wide)
        return runtimeError(
            "wide modifier is invalid for numeric DbgPrint formats");
      const bool Pointer = Kind == Conversion::Pointer;
      if (Pointer && (Spec.Length != LengthKind::None || Spec.Precision ||
                      Spec.Plus || Spec.Space || Spec.Alternate))
        return runtimeError("unsupported DbgPrint pointer format modifiers");
      const unsigned Bits = Pointer ? 64 : Spec.Bits;
      if (Bits < 64)
        Value &= (uint64_t(1) << Bits) - 1;
      bool Negative = false;
      if (Kind == Conversion::SignedDecimal) {
        Negative = Value & (uint64_t(1) << (Bits - 1));
        if (Negative) {
          Value = ~Value + 1;
          if (Bits < 64)
            Value &= (uint64_t(1) << Bits) - 1;
          Prefix = "-";
        } else if (Spec.Plus)
          Prefix = "+";
        else if (Spec.Space)
          Prefix = " ";
      }
      const bool Hex = Kind == Conversion::HexLower ||
                       Kind == Conversion::HexUpper || Pointer;
      const unsigned Base = Hex ? 16 : Kind == Conversion::Octal ? 8 : 10;
      const auto Digits =
          Kind == Conversion::HexUpper || Pointer ? UpperDigits : LowerDigits;
      uint64_t Remaining = Value;
      do {
        Text.push_back(Digits[Remaining % Base]);
        Remaining /= Base;
      } while (Remaining);
      std::reverse(Text.begin(), Text.end());
      const unsigned Precision = Pointer ? 16 : Spec.Precision.value_or(1);
      if (!Precision && !Value)
        Text.clear();
      if (Text.size() < Precision)
        Text.insert(0, Precision - Text.size(), '0');
      if (Spec.Alternate) {
        if (Hex && Value)
          Prefix = Kind == Conversion::HexUpper ? "0X" : "0x";
        else if (Kind == Conversion::Octal &&
                 (Text.empty() || Text.front() != '0'))
          Text.insert(Text.begin(), '0');
      }
    } else if (Kind == Conversion::String || Kind == Conversion::Character) {
      const bool Wide =
          Spec.Length == LengthKind::Wide || Spec.Length == LengthKind::Long;
      if (Wide && Model.currentIRQL() != scheduler::PassiveLevel)
        return runtimeError("Unicode DbgPrint requires IRQL PASSIVE_LEVEL");
      if (Spec.Length != LengthKind::None && Spec.Length != LengthKind::Short &&
          !Wide)
        return runtimeError(
            "unsupported DbgPrint character or string modifier");
      if (Kind == Conversion::String) {
        auto Result = string(Value, Wide, Spec.Precision);
        if (!Result)
          return Result.takeError();
        Text = std::move(*Result);
      } else {
        const uint64_t Unit = Wide ? uint16_t(Value) : uint8_t(Value);
        if (Unit > MaxASCII)
          return runtimeError(
              "DbgPrint non-ASCII character requires an unmodeled code page");
        Text.push_back(static_cast<char>(Unit));
      }
    } else {
      auto Result = countedString(Value, Spec);
      if (!Result)
        return Result.takeError();
      Text = std::move(*Result);
    }
    const size_t Size = Prefix.size() + Text.size();
    const size_t Padding = Spec.Width > Size ? Spec.Width - Size : 0;
    const bool PadZero = Numeric && Spec.Zero && !Spec.Left && !Spec.Precision;
    if (!Spec.Left && !PadZero)
      if (auto E = append(std::string(Padding, ' ')))
        return E;
    if (auto E = append(Prefix))
      return E;
    if (PadZero)
      if (auto E = append(std::string(Padding, '0')))
        return E;
    if (auto E = append(Text))
      return E;
    if (Spec.Left)
      return append(std::string(Padding, ' '));
    return llvm::Error::success();
  }

public:
  Formatter(const KernelModel &Model, GuestMemory &Memory,
            TypedArgumentReader ReadArgument, unsigned FirstArgument)
      : Model(Model), Memory(Memory), ReadArgument(ReadArgument),
        NextArgument(FirstArgument) {}

  llvm::Expected<std::string> format(uint64_t Address) {
    std::string Format;
    for (unsigned I = 0;; ++I) {
      if (I > MaxDebugFormatBytes || Address > UINT64_MAX - I)
        return runtimeError("unterminated DbgPrint format within model limit");
      auto Byte = readInteger(Model, Memory, Address + I, 1);
      if (!Byte)
        return Byte.takeError();
      if (!*Byte)
        break;
      if (*Byte > MaxASCII)
        return runtimeError("DbgPrint format must contain ASCII text");
      Format.push_back(static_cast<char>(*Byte));
    }
    for (size_t Position = 0; Position < Format.size();) {
      char Ch = Format[Position++];
      if (Ch != '%') {
        if (auto E = append(llvm::StringRef(&Ch, 1)))
          return E;
        continue;
      }
      if (Position < Format.size() && Format[Position] == '%') {
        ++Position;
        if (auto E = append("%"))
          return E;
        continue;
      }
      auto Spec = spec(Format, Position);
      if (!Spec)
        return Spec.takeError();
      if (Position == Format.size())
        return runtimeError(
            "DbgPrint ends with an incomplete format specifier");
      auto Kind = conversion(Format[Position++]);
      if (!Kind)
        return runtimeError("unsupported DbgPrint conversion specifier");
      // Slots advance by eight bytes on x64, but the kernel reads only the
      // conversion's scalar width (including byte/WORD character loads).
      unsigned Size = 8;
      if (*Kind < Conversion::Pointer)
        Size = Spec->Bits / 8;
      else if (*Kind == Conversion::Character)
        Size =
            Spec->Length == LengthKind::Wide || Spec->Length == LengthKind::Long
                ? 2
                : 1;
      auto Value = argument(Size);
      if (!Value)
        return Value.takeError();
      if (auto E = render(*Kind, *Spec, *Value))
        return E;
    }
    return Output;
  }
};

std::string debugBuffer(llvm::StringRef Prefix, llvm::StringRef Body) {
  // The prefix is clipped again for each growing attempt. An empty body can
  // therefore succeed with only 127 prefix bytes even when the prefix is long.
  for (unsigned Capacity = 128; Capacity <= MaxDebugBytes; Capacity += 128) {
    auto Kept = Prefix.take_front(Capacity - 1);
    if (Body.size() < Capacity - Kept.size()) {
      std::string Text = (Kept + Body).str();
      // Successful calls publish strlen(buffer), not the printf byte count.
      Text.resize(Text.find('\0') == std::string::npos ? Text.size()
                                                       : Text.find('\0'));
      return Text;
    }
  }
  // Overflow publishes exactly 511 bytes, even with embedded NUL characters.
  std::string Text = (Prefix.take_front(MaxDebugBytes - 1) + Body).str();
  Text.resize(MaxDebugBytes - 2);
  Text.push_back('\n');
  return Text;
}
} // namespace

llvm::Expected<uint64_t> unicodeOperation(const KernelModel &Model,
                                          GuestMemory &Memory,
                                          UnicodeOperation Operation,
                                          llvm::ArrayRef<uint64_t> A) {
  auto First = readCountedString(Model, Memory, A[0]);
  if (!First)
    return First.takeError();
  if (Operation == UnicodeOperation::Copy) {
    unsigned Length = 0;
    std::vector<uint8_t> Bytes;
    if (A[1]) {
      auto Source = readCountedString(Model, Memory, A[1]);
      if (!Source)
        return Source.takeError();
      Length = std::min(First->MaximumLength, Source->Length);
      auto Read = readBytes(Model, Memory, Source->Buffer, Length);
      if (!Read)
        return Read.takeError();
      Bytes = std::move(*Read);
      // A non-null source also terminates the result when a complete UTF-16
      // code unit fits. Length still describes only the copied source bytes.
      if (Length + 2 <= First->MaximumLength)
        Bytes.resize(Length + 2, 0);
    }
    if (auto E = Model.validateGuestAccess(A[0], 2, true))
      return E;
    if (auto E = Model.validateGuestAccess(First->Buffer, Bytes.size(), true))
      return E;
    if (!Bytes.empty())
      if (auto E = Memory.write(First->Buffer, Bytes))
        return E;
    if (auto E = Memory.writeInteger(A[0], Length, 2))
      return E;
    return 0;
  }
  if (static_cast<uint8_t>(A[2]) && Operation != UnicodeOperation::Prefix)
    return runtimeError("case-insensitive Unicode comparison requires an "
                        "unmodeled Windows case table");
  auto Second = readCountedString(Model, Memory, A[1]);
  if (!Second)
    return Second.takeError();
  if (Operation == UnicodeOperation::Prefix && First->Length > Second->Length)
    return 0;
  auto Left = readBytes(Model, Memory, First->Buffer, First->Length);
  if (!Left)
    return Left.takeError();
  auto Right = readBytes(
      Model, Memory, Second->Buffer,
      Operation == UnicodeOperation::Prefix ? First->Length : Second->Length);
  if (!Right)
    return Right.takeError();
  int32_t Difference = Operation == UnicodeOperation::Prefix
                           ? 0
                           : int32_t(First->Length) - Second->Length;
  for (size_t I = 0, End = std::min(Left->size(), Right->size()); I < End;
       I += 2) {
    uint16_t L = codeUnit(*Left, I), R = codeUnit(*Right, I);
    if (Operation == UnicodeOperation::Prefix && uint8_t(A[2])) {
      if (L > 0x7f || R > 0x7f)
        return runtimeError("case-insensitive Unicode prefix requires an "
                            "explicit NLS table outside ASCII");
      L = upperAnsiByte(uint8_t(L));
      R = upperAnsiByte(uint8_t(R));
    }
    if (L == R)
      continue;
    Difference = int32_t(L) - R;
    break;
  }
  if (Operation == UnicodeOperation::Equal ||
      Operation == UnicodeOperation::Prefix)
    return Difference == 0;
  return static_cast<uint32_t>(Difference);
}

llvm::Expected<uint64_t> initializeAnsiString(const KernelModel &Model,
                                              GuestMemory &Memory,
                                              llvm::ArrayRef<uint64_t> A) {
  // ntoskrnl (RVA 0x24fb00) scans to the terminator, truncates the counted
  // length to 0xFFFE and counts the terminator in MaximumLength.
  uint64_t Length = 0;
  if (A[1]) {
    for (;; ++Length) {
      if (auto E = checkBufferRange(A[1], Length + 1))
        return E;
      auto Byte = readInteger(Model, Memory, A[1] + Length, 1);
      if (!Byte)
        return Byte.takeError();
      if (!*Byte)
        break;
    }
    Length = std::min<uint64_t>(Length, 0xfffe);
  }
  // The routine clears the whole first quadword, including the padding.
  if (auto E = Model.validateGuestAccess(A[0], UnicodeRecordSize, true))
    return E;
  if (auto E = Memory.writeInteger(A[0], 0, 8))
    return E;
  if (auto E = Memory.writeInteger(A[0], Length, 2))
    return E;
  if (auto E = Memory.writeInteger(A[0] + UnicodeMaximumOffset,
                                   A[1] ? Length + 1 : 0, 2))
    return E;
  if (auto E = Memory.writeInteger(A[0] + UnicodeBufferOffset, A[1], 8))
    return E;
  return StatusSuccess;
}

llvm::Expected<uint64_t> initializeUnicodeStringEx(const KernelModel &Model,
                                                   GuestMemory &Memory,
                                                   llvm::ArrayRef<uint64_t> A) {
  // RVA 0x32eb60 clears the record and stores Buffer before measuring; a
  // source over 0x7FFE units fails with both lengths left zero.
  uint64_t Length = 0;
  if (A[1])
    for (;; Length += 2) {
      if (auto E = checkBufferRange(A[1], Length + 2))
        return E;
      auto Unit = readInteger(Model, Memory, A[1] + Length, 2);
      if (!Unit)
        return Unit.takeError();
      if (!*Unit)
        break;
    }
  if (auto E = Model.validateGuestAccess(A[0], UnicodeRecordSize, true))
    return E;
  if (auto E = Memory.writeInteger(A[0], 0, 8))
    return E;
  if (auto E = Memory.writeInteger(A[0] + UnicodeBufferOffset, A[1], 8))
    return E;
  if (!A[1])
    return StatusSuccess;
  if (Length > MaxUnicodeBytes)
    return StatusNameTooLong;
  if (auto E = Memory.writeInteger(A[0], Length, 2))
    return E;
  if (auto E = Memory.writeInteger(A[0] + UnicodeMaximumOffset, Length + 2, 2))
    return E;
  return StatusSuccess;
}

llvm::Expected<uint16_t> uppercaseUnicodeUnit(uint16_t Unit) {
  if (Unit >= 0xc0)
    return runtimeError("Unicode upcasing requires an unmodeled NLS table");
  return Unit >= 'a' && Unit <= 'z' ? uint16_t(Unit - 32) : Unit;
}

llvm::Expected<uint64_t> uppercaseCharacter(uint64_t Value, bool Narrow) {
  // Kernel toupper converts the low byte through the active code page, even
  // for int carriers outside the portable CRT input domain. ASCII is shared
  // by ANSI and UTF-8 modes; extended bytes require a declared code page.
  const uint16_t Unit = Narrow ? uint8_t(Value) : uint16_t(Value);
  if (Narrow && Unit > MaxASCII)
    return runtimeError("toupper requires an unmodeled ANSI code page");
  auto Upper = uppercaseUnicodeUnit(Unit);
  if (!Upper)
    return Upper.takeError();
  return uint64_t(*Upper);
}

llvm::Expected<uint64_t> hashUnicodeString(const KernelModel &Model,
                                           GuestMemory &Memory,
                                           llvm::ArrayRef<uint64_t> A) {
  if (!A[0] || !A[3])
    return StatusInvalidParameter;
  if (auto E = checkBufferRange(A[0], UnicodeBufferOffset + 8))
    return E;
  auto Buffer = readInteger(Model, Memory, A[0] + UnicodeBufferOffset, 8);
  if (!Buffer)
    return Buffer.takeError();
  if (auto E = Model.validateGuestAccess(A[3], 4, true))
    return E;
  if (auto E = Memory.writeInteger(A[3], 0, 4))
    return E;
  auto Length = readInteger(Model, Memory, A[0], 2);
  if (!Length)
    return Length.takeError();
  if (uint32_t(A[2]) > 1)
    return StatusInvalidParameter;
  uint32_t Hash = 0;
  for (unsigned I = 0; I < *Length / 2; ++I) {
    if (auto E = checkBufferRange(*Buffer, I * 2 + 2))
      return E;
    auto Unit = readInteger(Model, Memory, *Buffer + I * 2, 2);
    if (!Unit)
      return Unit.takeError();
    if (uint8_t(A[1])) {
      auto Upper = uppercaseUnicodeUnit(*Unit);
      if (!Upper)
        return Upper.takeError();
      *Unit = *Upper;
    }
    Hash = Hash * 65599u + uint32_t(*Unit);
  }
  if (auto E = Model.validateGuestAccess(A[3], 4, true))
    return E;
  if (auto E = Memory.writeInteger(A[3], Hash, 4))
    return E;
  return StatusSuccess;
}

llvm::Expected<uint64_t> timeToSecondsSince1980(const KernelModel &Model,
                                                GuestMemory &Memory,
                                                llvm::ArrayRef<uint64_t> A) {
  auto Time = readInteger(Model, Memory, A[0], 8);
  if (!Time)
    return Time.takeError();
  // Every negative signed input is before 1980. Unsigned subtraction makes
  // the other out-of-range values explicit without signed overflow.
  const uint64_t Seconds = *Time / 10000000 - 11960006400ULL;
  if ((*Time >> 63) || Seconds > UINT32_MAX)
    return 0;
  if (auto E = Model.validateGuestAccess(A[1], 4, true))
    return E;
  if (auto E = Memory.writeInteger(A[1], Seconds, 4))
    return E;
  return 1;
}

llvm::Expected<uint64_t> timeToTimeFields(const KernelModel &Model,
                                          GuestMemory &Memory,
                                          llvm::ArrayRef<uint64_t> A) {
  // RtlTimeToTimeFields without recorded leap seconds. A negative time feeds
  // the kernel's unsigned day arithmetic and has no calendar meaning.
  auto Time = readInteger(Model, Memory, A[0], 8);
  if (!Time)
    return Time.takeError();
  if (static_cast<int64_t>(*Time) < 0)
    return runtimeError("RtlTimeToTimeFields requires a non-negative time");
  const uint64_t Milliseconds = *Time / TimeUnitsPerMillisecond;
  const uint64_t Fraction = Milliseconds % MillisecondsPerDay;
  uint64_t Days = Milliseconds / MillisecondsPerDay;
  const uint64_t Weekday = (Days + 1) % 7;
  const uint64_t Cycles400 = Days / DaysPer400Years;
  Days %= DaysPer400Years;
  const uint64_t Cycles100 = std::min<uint64_t>(Days / DaysPer100Years, 3);
  Days -= Cycles100 * DaysPer100Years;
  const uint64_t Cycles4 = Days / DaysPer4Years;
  Days %= DaysPer4Years;
  const uint64_t Years = std::min<uint64_t>(Days / DaysPerYear, 3);
  Days -= Years * DaysPerYear;
  const uint64_t Year = FirstTimeFieldsYear + 400 * Cycles400 +
                        100 * Cycles100 + 4 * Cycles4 + Years;
  const auto &Preceding = DaysPrecedingMonth[isLeapYear(Year)];
  unsigned Month = 0;
  while (Days >= Preceding[Month + 1])
    ++Month;
  const uint64_t Fields[TimeFieldCount] = {Year,
                                           Month + 1U,
                                           Days - Preceding[Month] + 1,
                                           Fraction / 3600000,
                                           Fraction / 60000 % 60,
                                           Fraction / 1000 % 60,
                                           Fraction % 1000,
                                           Weekday};
  std::vector<uint8_t> Bytes(TimeFieldCount * 2);
  for (unsigned I = 0; I < TimeFieldCount; ++I) {
    Bytes[I * 2] = static_cast<uint8_t>(Fields[I]);
    Bytes[I * 2 + 1] = static_cast<uint8_t>(Fields[I] >> 8);
  }
  if (auto E = Model.validateGuestAccess(A[1], Bytes.size(), true))
    return E;
  if (auto E = Memory.write(A[1], Bytes))
    return E;
  return StatusSuccess;
}

llvm::Expected<uint64_t> timeFieldsToTime(const KernelModel &Model,
                                          GuestMemory &Memory,
                                          llvm::ArrayRef<uint64_t> A) {
  // RtlTimeFieldsToTime without recorded leap seconds ignores Weekday and
  // rejects fields outside the Gregorian calendar of 1601 through 30827.
  auto Bytes = readBytes(Model, Memory, A[0], (TimeFieldCount - 1) * 2);
  if (!Bytes)
    return Bytes.takeError();
  int64_t Field[TimeFieldCount - 1];
  for (unsigned I = 0; I + 1 < TimeFieldCount; ++I)
    Field[I] =
        static_cast<int16_t>((*Bytes)[I * 2] | ((*Bytes)[I * 2 + 1] << 8));
  const int64_t Year = Field[0], Month = Field[1], Day = Field[2];
  const int64_t Hour = Field[3], Minute = Field[4], Second = Field[5];
  const int64_t Milli = Field[6];
  if (Month < 1 || Month > 12 || Day < 1 || Year < FirstTimeFieldsYear ||
      Year > LastTimeFieldsYear)
    return 0;
  const auto &Preceding = DaysPrecedingMonth[isLeapYear(Year)];
  if (Day > Preceding[Month] - Preceding[Month - 1] || Hour < 0 || Hour > 23 ||
      Minute < 0 || Minute > 59 || Second < 0 || Second > 59 || Milli < 0 ||
      Milli > 999)
    return 0;
  const uint64_t Elapsed = Year - FirstTimeFieldsYear;
  const uint64_t Days = DaysPerYear * Elapsed + Elapsed / 4 - Elapsed / 100 +
                        Elapsed / 400 + Preceding[Month - 1] + Day - 1;
  const uint64_t Time = TimeUnitsPerMillisecond *
                        (Milli + 1000 * (Second + 60 * (Minute + 60 * Hour)) +
                         MillisecondsPerDay * Days);
  if (auto E = Model.validateGuestAccess(A[1], 8, true))
    return E;
  if (auto E = Memory.writeInteger(A[1], Time, 8))
    return E;
  return 1;
}

llvm::Expected<uint64_t> copyAnsiString(const KernelModel &Model,
                                        GuestMemory &Memory,
                                        llvm::ArrayRef<uint64_t> A) {
  // RtlCopyString copies what fits and, unlike the UTF-16 copy, never writes
  // a terminator. A null source only clears Length.
  if (!A[0])
    return runtimeError("STRING record must not be null");
  if (auto E = Model.validateGuestAccess(A[0], UnicodeRecordSize, true))
    return E;
  uint64_t Length = 0;
  if (A[1]) {
    auto Source = readCountedBytes(Model, Memory, A[1]);
    if (!Source)
      return Source.takeError();
    auto Maximum = Memory.readInteger(A[0] + UnicodeMaximumOffset, 2);
    if (!Maximum)
      return Maximum.takeError();
    Length = std::min<uint64_t>(Source->Length, *Maximum);
    auto Bytes = readBytes(Model, Memory, Source->Buffer, Length);
    if (!Bytes)
      return Bytes.takeError();
    auto Buffer = Memory.readInteger(A[0] + UnicodeBufferOffset, 8);
    if (!Buffer)
      return Buffer.takeError();
    if (auto E = Model.validateGuestAccess(*Buffer, Bytes->size(), true))
      return E;
    if (!Bytes->empty())
      if (auto E = Memory.write(*Buffer, *Bytes))
        return E;
  }
  if (auto E = Memory.writeInteger(A[0], Length, 2))
    return E;
  return StatusSuccess;
}

llvm::Expected<uint64_t> compareAnsiStrings(const KernelModel &Model,
                                            GuestMemory &Memory,
                                            llvm::ArrayRef<uint64_t> A,
                                            bool EqualityOnly) {
  if (EqualityOnly) {
    auto Left = readInteger(Model, Memory, A[0], 2);
    if (!Left)
      return Left.takeError();
    auto Right = readInteger(Model, Memory, A[1], 2);
    if (!Right)
      return Right.takeError();
    if (*Left != *Right)
      return 0;
  }
  auto Compared = compareAnsiStrings(Model, Memory, A);
  if (!Compared)
    return Compared.takeError();
  return EqualityOnly ? uint64_t(*Compared == 0) : *Compared;
}

llvm::Expected<uint64_t> appendUnicodeString(const KernelModel &Model,
                                             GuestMemory &Memory,
                                             llvm::ArrayRef<uint64_t> A,
                                             bool CountedSource) {
  uint64_t Source = A[1];
  uint64_t Length = 0;
  if (CountedSource) {
    // An empty counted source succeeds before the destination is read
    // (RVA 0x3480c0).
    auto Record = readCountedString(Model, Memory, A[1]);
    if (!Record)
      return Record.takeError();
    if (!Record->Length)
      return StatusSuccess;
    Source = Record->Buffer;
    Length = Record->Length;
  } else {
    // A null source is an empty append (RVA 0x32ead0). Others are measured
    // like RtlInitUnicodeStringEx (RVA 0x32eb60), which rejects sources
    // longer than 0x7FFE units; the append then reports a short buffer.
    if (!Source)
      return StatusSuccess;
    for (;; Length += 2) {
      if (auto E = checkBufferRange(Source, Length + 2))
        return E;
      auto Unit = readInteger(Model, Memory, Source + Length, 2);
      if (!Unit)
        return Unit.takeError();
      if (!*Unit)
        break;
    }
    if (Length > MaxUnicodeBytes)
      return StatusBufferTooSmall;
  }
  auto Destination = readCountedString(Model, Memory, A[0]);
  if (!Destination)
    return Destination.takeError();
  const uint64_t Combined = Destination->Length + Length;
  if (Combined > Destination->MaximumLength)
    return StatusBufferTooSmall;
  auto Bytes = readBytes(Model, Memory, Source, Length);
  if (!Bytes)
    return Bytes.takeError();
  // The result is terminated only when a complete UTF-16 unit still fits.
  if (Combined + 1 < Destination->MaximumLength)
    Bytes->resize(Length + 2, 0);
  const uint64_t Target = Destination->Buffer + Destination->Length;
  if (auto E = Model.validateGuestAccess(A[0], 2, true))
    return E;
  if (auto E = Model.validateGuestAccess(Target, Bytes->size(), true))
    return E;
  if (!Bytes->empty())
    if (auto E = Memory.write(Target, *Bytes))
      return E;
  if (auto E = Memory.writeInteger(A[0], Combined, 2))
    return E;
  return StatusSuccess;
}

llvm::Expected<uint64_t> prefixUnicodeString(const KernelModel &Model,
                                             GuestMemory &Memory,
                                             llvm::ArrayRef<uint64_t> A) {
  return unicodeOperation(Model, Memory, UnicodeOperation::Prefix, A);
}

llvm::Expected<uint64_t> integerToUnicodeString(const KernelModel &Model,
                                                GuestMemory &Memory,
                                                llvm::ArrayRef<uint64_t> A,
                                                bool WideValue) {
  return integerUnicodeString(Model, Memory, A, WideValue);
}

llvm::Expected<uint64_t> unicodeStringToInteger(const KernelModel &Model,
                                                GuestMemory &Memory,
                                                llvm::ArrayRef<uint64_t> A) {
  return unicodeToInteger(Model, Memory, A);
}

namespace {
// Read one 1- or 2-byte unit at \p Base + \p Index units, validating the exact
// unit and refusing a terminator-less run once the offset passes 1 MiB.
llvm::Expected<uint64_t> readScanUnit(const KernelModel &Model,
                                      GuestMemory &Memory, uint64_t Base,
                                      uint64_t Index, unsigned UnitWidth) {
  if (auto E = checkBufferRange(Base, (Index + 1) * UnitWidth))
    return E;
  return readInteger(Model, Memory, Base + Index * UnitWidth, UnitWidth);
}

// The kernel folds only ASCII A-Z to lowercase for case-insensitive compares.
uint64_t foldAsciiUpper(uint64_t Unit) {
  return static_cast<uint32_t>(Unit - 'A') < 26u ? Unit + 32 : Unit;
}
} // namespace

llvm::Expected<uint64_t> imageNtHeader(const KernelModel &Model,
                                       GuestMemory &Memory, uint64_t Base) {
  // RtlImageNtHeader calls RtlImageNtHeaderEx with NO_RANGE_CHECK. It validates
  // DOS and PE signatures, not the machine, optional header or section table.
  if (!Base || Base == UINT64_MAX)
    return 0;
  auto Read = [&](uint64_t Address,
                  unsigned Width) -> llvm::Expected<uint64_t> {
    if (auto E = checkBufferRange(Address, Width))
      return E;
    if (auto E = Model.validateGuestAccess(Address, Width, false))
      return E;
    return Memory.readInteger(Address, Width);
  };
  auto Dos = Read(Base, 2);
  if (!Dos)
    return Dos.takeError();
  if (*Dos != 0x5a4d)
    return 0;
  if (Base > UINT64_MAX - 60)
    return runtimeError("DOS header address overflows");
  auto Offset = Read(Base + 60, 4);
  if (!Offset)
    return Offset.takeError();
  if (*Offset > UINT64_MAX - Base)
    return 0;
  const uint64_t Header = Base + *Offset;
  // The pinned x64 routine applies the original user-address fence to a
  // low-address image, even though NO_RANGE_CHECK omits the caller image size.
  constexpr uint64_t UserFence = 0x7ffffffeffff;
  if (Base < UserFence && Header >= UserFence - 264)
    return 0;
  auto Signature = Read(Header, 4);
  if (!Signature)
    return Signature.takeError();
  return *Signature == 0x4550 ? Header : 0;
}

uint64_t cIsDigit(uint32_t Value) {
  const uint8_t Byte = uint8_t(Value);
  return Value != UINT32_MAX && Byte >= '0' && Byte <= '9' ? 4 : 0;
}

llvm::Expected<uint64_t> cStringChangeCase(const KernelModel &Model,
                                           GuestMemory &Memory,
                                           uint64_t Address, unsigned UnitWidth,
                                           bool Lower) {
  // The narrow upper and wide lower exports handle NULL; wide upper reads it.
  if (!Address && (UnitWidth == 1 || Lower))
    return 0;
  const uint64_t First = Lower ? 'A' : 'a';
  for (uint64_t I = 0;; ++I) {
    auto Unit = readScanUnit(Model, Memory, Address, I, UnitWidth);
    if (!Unit)
      return Unit.takeError();
    if (!*Unit)
      return Address;
    if (*Unit >= First && *Unit < First + 26) {
      const uint64_t Target = Address + I * UnitWidth;
      if (auto E = Model.validateGuestAccess(Target, UnitWidth, true))
        return E;
      if (auto E = Memory.writeInteger(Target, Lower ? *Unit + 32 : *Unit - 32,
                                       UnitWidth))
        return E;
    }
  }
}

llvm::Expected<uint64_t> cStringCheckedWrite(const KernelModel &Model,
                                             GuestMemory &Memory,
                                             llvm::ArrayRef<uint64_t> A,
                                             unsigned UnitWidth, bool Append) {
  constexpr uint64_t Invalid = 22, Range = 34, Truncated = 80;
  const bool Counted = A.size() == 4;
  const uint64_t Destination = A[0], Capacity = A[1], Source = A[2];
  const uint64_t Count = Counted ? A[3] : UINT64_MAX;
  if (Counted && !Count && !Destination && !Capacity)
    return 0;
  if (!Destination || !Capacity)
    return Invalid;
  auto Store = [&](uint64_t Index, uint64_t Value) -> llvm::Error {
    if (Index >= profile::KernelArenaSize / UnitWidth)
      return runtimeError("checked C string exceeds the model buffer limit");
    if (auto E = checkBufferRange(Destination, (Index + 1) * UnitWidth))
      return E;
    const uint64_t Address = Destination + Index * UnitWidth;
    if (auto E = Model.validateGuestAccess(Address, UnitWidth, true))
      return E;
    return Memory.writeInteger(Address, Value, UnitWidth);
  };
  auto Clear = [&](uint64_t Status) -> llvm::Expected<uint64_t> {
    if (auto E = Store(0, 0))
      return E;
    return Status;
  };
  if (Counted && !Count)
    return Clear(0);
  if (!Source)
    return Clear(Invalid);
  uint64_t Prefix = 0;
  if (Append) {
    while (Prefix < Capacity) {
      auto Unit = readScanUnit(Model, Memory, Destination, Prefix, UnitWidth);
      if (!Unit)
        return Unit.takeError();
      if (!*Unit)
        break;
      ++Prefix;
    }
    if (Prefix == Capacity)
      return Clear(Invalid);
  }
  if (Append && Destination == Source)
    return runtimeError("overlapping secure strings have undefined behavior");
  const uint64_t Available = Capacity - Prefix;
  for (uint64_t I = 0;; ++I) {
    auto Unit = readScanUnit(Model, Memory, Source, I, UnitWidth);
    if (!Unit)
      return Unit.takeError();
    // These exports copy one unit at a time, including on error. A later
    // fault does not roll back earlier stores or a completed append prefix.
    if (auto E = Store(Prefix + I, *Unit))
      return E;
    if (!*Unit)
      return 0;
    if (I + 1 == Available) {
      if (Counted && Count == UINT64_MAX) {
        if (auto E = Store(Capacity - 1, 0))
          return E;
        return Truncated;
      }
      return Clear(Range);
    }
    if (Counted && I + 1 == Count) {
      if (auto E = Store(Prefix + I + 1, 0))
        return E;
      return 0;
    }
  }
}

llvm::Expected<uint64_t> cStringLength(const KernelModel &Model,
                                       GuestMemory &Memory,
                                       llvm::ArrayRef<uint64_t> A,
                                       unsigned UnitWidth) {
  const bool Bounded = A.size() > 1;
  for (uint64_t Length = 0; !Bounded || Length < A[1]; ++Length) {
    auto Unit = readScanUnit(Model, Memory, A[0], Length, UnitWidth);
    if (!Unit)
      return Unit.takeError();
    if (!*Unit)
      return Length;
  }
  // A satisfied count stops without reading the terminator.
  return A[1];
}

llvm::Expected<uint64_t> cStringCompare(const KernelModel &Model,
                                        GuestMemory &Memory,
                                        llvm::ArrayRef<uint64_t> A,
                                        unsigned UnitWidth,
                                        CStringCompare Kind) {
  const bool Counted = A.size() > 2;
  const bool Fold = Kind == CStringCompare::FoldedRaw;
  for (uint64_t Index = 0; !Counted || Index < A[2]; ++Index) {
    auto Left = readScanUnit(Model, Memory, A[0], Index, UnitWidth);
    if (!Left)
      return Left.takeError();
    auto Right = readScanUnit(Model, Memory, A[1], Index, UnitWidth);
    if (!Right)
      return Right.takeError();
    uint64_t Lv = Fold ? foldAsciiUpper(*Left) : *Left;
    uint64_t Rv = Fold ? foldAsciiUpper(*Right) : *Right;
    if (Lv != Rv) {
      if (Kind == CStringCompare::Normalized)
        return Lv < Rv ? uint64_t(uint32_t(-1)) : uint64_t(1);
      return uint64_t(static_cast<uint32_t>(static_cast<int64_t>(Lv) -
                                            static_cast<int64_t>(Rv)));
    }
    if (!Lv)
      return 0;
  }
  return 0;
}

llvm::Expected<uint64_t> cStringWrite(const KernelModel &Model,
                                      GuestMemory &Memory,
                                      llvm::ArrayRef<uint64_t> A,
                                      unsigned UnitWidth, bool Append) {
  const bool Counted = A.size() == 3;
  const uint64_t Count = Counted ? A[2] : UINT64_MAX;
  // Unlike strncat, the wide kernel routine still scans and stores the
  // destination terminator when Count is zero. Neither reads the source.
  if (Counted && !Count && (!Append || UnitWidth == 1))
    return A[0];
  if (Counted && !Append) {
    if (Count > UINT64_MAX / UnitWidth)
      return runtimeError("C string count overflows its byte size");
    if (auto E = checkBufferRange(A[0], Count * UnitWidth))
      return E;
  }
  uint64_t PrefixBytes = 0;
  if (Append) {
    auto Length = cStringLength(Model, Memory, {A[0]}, UnitWidth);
    if (!Length)
      return Length.takeError();
    PrefixBytes = *Length * UnitWidth;
  }
  std::vector<uint8_t> Output;
  bool Terminated = false;
  for (uint64_t I = 0; I < Count; ++I) {
    auto Unit = readScanUnit(Model, Memory, A[1], I, UnitWidth);
    if (!Unit)
      return Unit.takeError();
    Output.push_back(uint8_t(*Unit));
    if (UnitWidth == 2)
      Output.push_back(uint8_t(*Unit >> 8));
    if (!*Unit) {
      Terminated = true;
      break;
    }
  }
  const uint64_t SourceBytes = Output.size();
  if (Counted && !Append)
    Output.resize(Count * UnitWidth, 0);
  else if (!Terminated)
    Output.resize(Output.size() + UnitWidth, 0);
  const uint64_t TotalBytes = PrefixBytes + Output.size();
  if (auto E = checkBufferRange(A[0], TotalBytes))
    return E;
  // C defines no overlapping copy/append behavior. Include the destination
  // prefix in that check, but only source units actually consumed by the call.
  if (SourceBytes &&
      (A[0] <= A[1] ? A[1] - A[0] < TotalBytes : A[0] - A[1] < SourceBytes))
    return runtimeError("overlapping buffers passed to a C string write API");
  const uint64_t Destination = A[0] + PrefixBytes;
  if (auto E = Model.validateGuestAccess(Destination, Output.size(), true))
    return E;
  auto Writable = Memory.canAccess(Destination, Output.size(), Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return runtimeError("C string destination requires writable memory");
  if (auto E = Memory.validateBacking(Destination, Output.size()))
    return E;
  if (auto E = Memory.write(Destination, Output))
    return E;
  return A[0];
}

llvm::Expected<uint64_t> cStringFindChar(const KernelModel &Model,
                                         GuestMemory &Memory,
                                         llvm::ArrayRef<uint64_t> A,
                                         unsigned UnitWidth, bool Reverse) {
  const uint64_t Target = A[1] & (UnitWidth == 1 ? 0xffu : 0xffffu);
  uint64_t Match = 0;
  for (uint64_t Index = 0;; ++Index) {
    auto Unit = readScanUnit(Model, Memory, A[0], Index, UnitWidth);
    if (!Unit)
      return Unit.takeError();
    if (*Unit == Target) {
      const uint64_t Here = A[0] + Index * UnitWidth;
      if (!Reverse)
        return Here;
      Match = Here;
    }
    if (!*Unit)
      return Reverse ? Match : 0;
  }
}

llvm::Expected<uint64_t> cStringFind(const KernelModel &Model,
                                     GuestMemory &Memory,
                                     llvm::ArrayRef<uint64_t> A,
                                     unsigned UnitWidth) {
  const auto scanLength = [&](uint64_t Base) -> llvm::Expected<uint64_t> {
    for (uint64_t Length = 0;; ++Length) {
      auto Unit = readScanUnit(Model, Memory, Base, Length, UnitWidth);
      if (!Unit)
        return Unit.takeError();
      if (!*Unit)
        return Length;
    }
  };
  auto NeedleLen = scanLength(A[1]);
  if (!NeedleLen)
    return NeedleLen.takeError();
  // An empty needle returns the haystack before the haystack is examined.
  if (!*NeedleLen)
    return A[0];
  auto HayLen = scanLength(A[0]);
  if (!HayLen)
    return HayLen.takeError();
  if (*HayLen < *NeedleLen)
    return 0;
  auto Needle = readBytes(Model, Memory, A[1], *NeedleLen * UnitWidth);
  if (!Needle)
    return Needle.takeError();
  auto Hay = readBytes(Model, Memory, A[0], *HayLen * UnitWidth);
  if (!Hay)
    return Hay.takeError();
  for (uint64_t Start = 0; Start + *NeedleLen <= *HayLen; ++Start)
    if (std::equal(Needle->begin(), Needle->end(),
                   Hay->begin() + Start * UnitWidth))
      return A[0] + Start * UnitWidth;
  return 0;
}

llvm::Expected<uint64_t> cStringSpan(const KernelModel &Model,
                                     GuestMemory &Memory,
                                     llvm::ArrayRef<uint64_t> A,
                                     unsigned UnitWidth, bool Complement) {
  uint64_t ControlLength = 0;
  for (;; ++ControlLength) {
    auto Unit = readScanUnit(Model, Memory, A[1], ControlLength, UnitWidth);
    if (!Unit)
      return Unit.takeError();
    if (!*Unit)
      break;
  }
  auto Control = readBytes(Model, Memory, A[1], ControlLength * UnitWidth);
  if (!Control)
    return Control.takeError();
  std::vector<uint64_t> Set;
  Set.reserve(ControlLength);
  for (uint64_t I = 0; I < ControlLength; ++I)
    Set.push_back(UnitWidth == 1 ? (*Control)[I] : codeUnit(*Control, I * 2));
  const auto inSet = [&](uint64_t Unit) {
    return std::find(Set.begin(), Set.end(), Unit) != Set.end();
  };
  for (uint64_t Index = 0;; ++Index) {
    auto Unit = readScanUnit(Model, Memory, A[0], Index, UnitWidth);
    if (!Unit)
      return Unit.takeError();
    if (!*Unit || (Complement ? inSet(*Unit) : !inSet(*Unit)))
      return Index;
  }
}

llvm::Expected<uint64_t> appendUnicode(const KernelModel &Model,
                                       GuestMemory &Memory,
                                       llvm::ArrayRef<uint64_t> A) {
  if (!A[1])
    return 0;
  std::vector<uint8_t> Text;
  bool Terminated = false;
  for (unsigned I = 0; I < MaxCountedUnicodeBytes; I += 2) {
    if (A[1] > UINT64_MAX - I)
      return runtimeError("Unicode append source address overflows");
    auto Unit = readInteger(Model, Memory, A[1] + I, 2);
    if (!Unit)
      return Unit.takeError();
    if (!*Unit) {
      Terminated = true;
      break;
    }
    Text.push_back(uint8_t(*Unit));
    Text.push_back(uint8_t(*Unit >> 8));
  }
  if (!Terminated)
    return runtimeError(
        "Unicode append source exceeds bounded terminated string");
  auto Destination = readCountedString(Model, Memory, A[0]);
  if (!Destination)
    return Destination.takeError();
  const uint64_t Length = Destination->Length + Text.size();
  if (Length > Destination->MaximumLength)
    return uint64_t(0xc0000023);
  if (Length + 2 <= Destination->MaximumLength) {
    Text.push_back(0);
    Text.push_back(0);
  }
  if (Destination->Buffer > UINT64_MAX - Destination->Length)
    return runtimeError("Unicode append destination address overflows");
  if (auto E = Model.validateGuestAccess(A[0], 2, true))
    return E;
  const uint64_t Output = Destination->Buffer + Destination->Length;
  if (auto E = Model.validateGuestAccess(Output, Text.size(), true))
    return E;
  auto Header = Memory.pinBacking(A[0], 16);
  if (!Header)
    return Header.takeError();
  if (!Text.empty()) {
    auto Data = Memory.pinBacking(Output, Text.size());
    if (!Data)
      return Data.takeError();
    if (Data->overlaps(*Header))
      return runtimeError(
          "Unicode append output aliases its destination record");
  }
  if (!Text.empty())
    if (auto E = Memory.write(Output, Text))
      return E;
  if (auto E = Memory.writeInteger(A[0], Length, 2))
    return E;
  return 0;
}

namespace {
int digit(uint16_t Unit) {
  if (Unit >= '0' && Unit <= '9')
    return Unit - '0';
  if (Unit >= 'a' && Unit <= 'f')
    return Unit - 'a' + 10;
  if (Unit >= 'A' && Unit <= 'F')
    return Unit - 'A' + 10;
  return -1;
}
llvm::Expected<uint32_t>
parseInteger(uint32_t Base,
             llvm::function_ref<llvm::Expected<uint16_t>(size_t)> Read) {
  size_t I = 0;
  auto Unit = Read(I);
  if (!Unit)
    return Unit.takeError();
  while (*Unit <= 0x20) {
    Unit = Read(++I);
    if (!Unit)
      return Unit.takeError();
  }
  bool Negative = false;
  if (*Unit == '+' || *Unit == '-') {
    Negative = *Unit == '-';
    Unit = Read(++I);
    if (!Unit)
      return Unit.takeError();
  }
  if (!Base) {
    Base = 10;
    if (*Unit == '0') {
      auto Prefix = Read(I + 1);
      if (!Prefix)
        return Prefix.takeError();
      if (*Prefix == 'x' || *Prefix == 'b' || *Prefix == 'o') {
        Base = *Prefix == 'x' ? 16 : *Prefix == 'b' ? 2 : 8;
        I += 2;
        Unit = Read(I);
        if (!Unit)
          return Unit.takeError();
      }
    }
  }
  uint32_t Value = 0;
  while (true) {
    const int Digit = digit(*Unit);
    if (Digit < 0 || uint32_t(Digit) >= Base)
      break;
    Value = Value * Base + uint32_t(Digit);
    Unit = Read(++I);
    if (!Unit)
      return Unit.takeError();
  }
  return Negative ? uint32_t(0) - Value : Value;
}
llvm::Error rejectInputOutputOverlap(GuestMemory &Memory, uint64_t Record,
                                     const CountedString &S, uint64_t Output,
                                     unsigned Size) {
  auto Overlap = [&](uint64_t Address, uint64_t Count) {
    return Count &&
           (Output > UINT64_MAX - Size || Address > UINT64_MAX - Count ||
            (Output < Address + Count && Address < Output + Size));
  };
  if (Overlap(Record, 16) || Overlap(S.Buffer, S.Length))
    return runtimeError(
        "overlapping RTL parser input and output are unmodeled");
  auto Out = Memory.pinBacking(Output, Size);
  if (!Out)
    return Out.takeError();
  auto Header = Memory.pinBacking(Record, 16);
  if (!Header)
    return Header.takeError();
  if (Out->overlaps(*Header))
    return runtimeError("aliased RTL parser input and output are unmodeled");
  if (S.Length) {
    auto Text = Memory.pinBacking(S.Buffer, S.Length);
    if (!Text)
      return Text.takeError();
    if (Out->overlaps(*Text))
      return runtimeError("aliased RTL parser input and output are unmodeled");
  }
  return llvm::Error::success();
}
} // namespace

llvm::Expected<uint64_t> unicodeToInteger(const KernelModel &Model,
                                          GuestMemory &Memory,
                                          llvm::ArrayRef<uint64_t> A) {
  if (auto E = Model.validateGuestAccess(A[0], UnicodeRecordSize, false))
    return E;
  auto Length = Memory.readInteger(A[0], 2);
  if (!Length)
    return Length.takeError();
  auto Buffer = Memory.readInteger(A[0] + UnicodeBufferOffset, 8);
  if (!Buffer)
    return Buffer.takeError();
  const CountedString Captured{uint16_t(*Length), UINT16_MAX, *Buffer};
  const auto *Source = &Captured;
  if (auto E = rejectInputOutputOverlap(Memory, A[0], *Source, A[2], 4))
    return E;
  if (auto E = Model.validateGuestAccess(A[2], 4, true))
    return E;
  if (auto E = Memory.writeInteger(A[2], 0, 4))
    return E;
  if (!Source->Length || (Source->Length & 1))
    return StatusInvalidParameter;
  uint32_t Base = uint32_t(A[1]);
  if (Base && Base != 2 && Base != 8 && Base != 10 && Base != 16)
    return StatusInvalidParameter;
  auto Bytes = readBytes(Model, Memory, Source->Buffer, Source->Length);
  if (!Bytes)
    return Bytes.takeError();
  auto Value = parseInteger(Base, [&](size_t I) -> llvm::Expected<uint16_t> {
    return I < Bytes->size() / 2 ? codeUnit(*Bytes, I * 2) : uint16_t(0xffff);
  });
  if (!Value)
    return Value.takeError();
  if (auto E = Memory.writeInteger(A[2], *Value, 4))
    return E;
  return 0;
}

llvm::Expected<uint64_t> charToInteger(const KernelModel &Model,
                                       GuestMemory &Memory,
                                       llvm::ArrayRef<uint64_t> A) {
  const uint32_t Base = uint32_t(A[1]);
  // Unlike the counted Unicode entry, this native entry rejects the radix
  // before reading the source or touching the output.
  if (Base && Base != 2 && Base != 8 && Base != 10 && Base != 16)
    return StatusInvalidParameter;
  auto Value = parseInteger(Base, [&](size_t I) -> llvm::Expected<uint16_t> {
    if (I >= MaxCountedUnicodeBytes || A[0] > UINT64_MAX - I)
      return runtimeError(
          "RTL character integer input exceeds bounded readable bytes");
    auto Byte = readInteger(Model, Memory, A[0] + I, 1);
    if (!Byte)
      return Byte.takeError();
    return uint16_t(*Byte);
  });
  if (!Value)
    return Value.takeError();
  if (auto E = Model.validateGuestAccess(A[2], 4, true))
    return E;
  if (auto E = Memory.writeInteger(A[2], *Value, 4))
    return E;
  return 0;
}

llvm::Expected<uint64_t> guidFromUnicode(const KernelModel &Model,
                                         GuestMemory &Memory,
                                         llvm::ArrayRef<uint64_t> A) {
  auto Source = readCountedString(Model, Memory, A[0]);
  if (!Source)
    return Source.takeError();
  if (auto E = rejectInputOutputOverlap(Memory, A[0], *Source, A[1], 16))
    return E;
  auto Bytes = readBytes(Model, Memory, Source->Buffer, Source->Length);
  if (!Bytes)
    return Bytes.takeError();
  auto Unit = [&](size_t I) {
    return I * 2 < Bytes->size() ? codeUnit(*Bytes, I * 2) : 0;
  };
  if (Unit(0) != '{')
    return StatusInvalidParameter;
  size_t I = 1;
  auto Field = [&](unsigned Digits) -> std::optional<uint32_t> {
    uint32_t Value = 0;
    for (unsigned N = 0; N < Digits; ++N) {
      const int D = digit(Unit(I++));
      if (D < 0)
        return std::nullopt;
      Value = (Value << 4) | uint32_t(D);
    }
    return Value;
  };
  for (auto [Digits, Offset, Width] :
       {std::tuple{8u, 0u, 4u}, std::tuple{4u, 4u, 2u},
        std::tuple{4u, 6u, 2u}}) {
    auto Value = Field(Digits);
    if (!Value)
      return StatusInvalidParameter;
    if (auto E = Model.validateGuestAccess(A[1] + Offset, Width, true))
      return E;
    if (auto E = Memory.writeInteger(A[1] + Offset, *Value, Width))
      return E;
    if (Unit(I++) != '-')
      return StatusInvalidParameter;
  }
  std::array<uint8_t, 8> Tail;
  for (unsigned N = 0; N < 8; ++N) {
    if (N == 2 && Unit(I++) != '-')
      return StatusInvalidParameter;
    auto Value = Field(2);
    if (!Value)
      return StatusInvalidParameter;
    Tail[N] = uint8_t(*Value);
  }
  if (Unit(I++) != '}' || Unit(I))
    return StatusInvalidParameter;
  if (auto E = Model.validateGuestAccess(A[1] + 8, 8, true))
    return E;
  if (auto E = Memory.write(A[1] + 8, Tail))
    return E;
  return 0;
}

llvm::Expected<std::string> formatDebugMessage(const KernelModel &Model,
                                               GuestMemory &Memory,
                                               uint64_t Format,
                                               unsigned FirstArgument,
                                               ArgumentReader ReadArgument) {
  auto Read = [&](unsigned Index, unsigned) -> llvm::Expected<uint64_t> {
    if (!ReadArgument)
      return runtimeError("DbgPrint variadic argument reader is unavailable");
    return ReadArgument(Index);
  };
  auto Body = Formatter(Model, Memory, Read, FirstArgument).format(Format);
  if (!Body)
    return Body.takeError();
  return debugBuffer({}, *Body);
}

llvm::Expected<std::string> formatPrefixedDebugMessage(const KernelModel &Model,
                                                       GuestMemory &Memory,
                                                       uint64_t Prefix,
                                                       uint64_t Format,
                                                       uint64_t Arguments) {
  std::string Text;
  // strlen scans the whole prefix, even if only a small prefix slice is kept.
  for (uint64_t I = 0;; ++I) {
    if (auto E = checkBufferRange(Prefix, I + 1))
      return E;
    auto Byte = readInteger(Model, Memory, Prefix + I, 1);
    if (!Byte)
      return Byte.takeError();
    if (!*Byte)
      break;
    if (*Byte > MaxASCII)
      return runtimeError("DbgPrint prefix requires an unmodeled code page");
    if (Text.size() < MaxDebugBytes - 1)
      Text.push_back(static_cast<char>(*Byte));
  }
  auto Read = [&](unsigned Index, unsigned Size) -> llvm::Expected<uint64_t> {
    const uint64_t Offset = uint64_t(Index) * 8;
    if (Arguments > UINT64_MAX - Offset)
      return runtimeError("DbgPrint va_list address overflows");
    if (auto E = checkBufferRange(Arguments + Offset, Size))
      return E;
    return readInteger(Model, Memory, Arguments + Offset, Size);
  };
  auto Body = Formatter(Model, Memory, Read, 0).format(Format);
  if (!Body)
    return Body.takeError();
  return debugBuffer(Text, *Body);
}
} // namespace neverd::emulation::runtime
