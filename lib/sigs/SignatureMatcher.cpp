//===- SignatureMatcher.cpp - FLIRT pattern matching engine ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sigs/SignatureMatcher.h"

#include <algorithm>

using namespace neverd::sigs;

// CRC16 lookup table (CCITT reversed polynomial 0x8408).
static const uint16_t CRC16Table[256] = {
    0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241, 0xC601,
    0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440, 0xCC01, 0x0CC0,
    0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40, 0x0A00, 0xCAC1, 0xCB81,
    0x0B40, 0xC901, 0x09C0, 0x0880, 0xC841, 0xD801, 0x18C0, 0x1980, 0xD941,
    0x1B00, 0xDBC1, 0xDA81, 0x1A40, 0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01,
    0x1DC0, 0x1C80, 0xDC41, 0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0,
    0x1680, 0xD641, 0xD201, 0x12C0, 0x1380, 0xD341, 0x1100, 0xD1C1, 0xD081,
    0x1040, 0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240,
    0x3600, 0xF6C1, 0xF781, 0x3740, 0xF501, 0x35C0, 0x3480, 0xF441, 0x3C00,
    0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41, 0xFA01, 0x3AC0,
    0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840, 0x2800, 0xE8C1, 0xE981,
    0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41, 0xEE01, 0x2EC0, 0x2F80, 0xEF41,
    0x2D00, 0xEDC1, 0xEC81, 0x2C40, 0xE401, 0x24C0, 0x2580, 0xE541, 0x2700,
    0xE7C1, 0xE681, 0x2640, 0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0,
    0x2080, 0xE041, 0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281,
    0x6240, 0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441,
    0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0, 0x6E80, 0xAE41, 0xAA01,
    0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840, 0x7800, 0xB8C1,
    0xB981, 0x7940, 0xBB01, 0x7BC0, 0x7A80, 0xBA41, 0xBE01, 0x7EC0, 0x7F80,
    0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40, 0xB401, 0x74C0, 0x7580, 0xB541,
    0x7700, 0xB7C1, 0xB681, 0x7640, 0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101,
    0x71C0, 0x7080, 0xB041, 0x5000, 0x90C1, 0x9181, 0x5140, 0x9301, 0x53C0,
    0x5280, 0x9241, 0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481,
    0x5440, 0x9C01, 0x5CC0, 0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40,
    0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841, 0x8801,
    0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40, 0x4E00, 0x8EC1,
    0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41, 0x4400, 0x84C1, 0x8581,
    0x4540, 0x8701, 0x47C0, 0x4680, 0x8641, 0x8201, 0x42C0, 0x4380, 0x8341,
    0x4100, 0x81C1, 0x8081, 0x4040,
};

uint16_t SignatureMatcher::computeCRC16(const uint8_t *Data, size_t Len) {
  uint16_t CRC = 0xFFFF;
  for (size_t I = 0; I < Len; ++I) {
    uint8_t Idx = static_cast<uint8_t>(CRC ^ Data[I]);
    CRC = (CRC >> 8) ^ CRC16Table[Idx];
  }
  return CRC;
}

bool SignatureMatcher::matchLeading(const std::vector<PatternByte> &Pattern,
                                    const uint8_t *Data, size_t Available) {
  if (Pattern.size() > Available)
    return false;
  for (size_t I = 0; I < Pattern.size(); ++I) {
    if (Pattern[I].IsWildcard)
      continue;
    if (Data[I] != Pattern[I].Value)
      return false;
  }
  return true;
}

bool SignatureMatcher::verifyCRC(const PatternModule &Mod, const uint8_t *Data,
                                 size_t Available) {
  if (Mod.CRCLen == 0)
    return true;
  size_t CRCStart = Mod.LeadingBytes.size();
  size_t CRCEnd = CRCStart + Mod.CRCLen;
  if (CRCEnd > Available)
    return false;
  return computeCRC16(Data + CRCStart, Mod.CRCLen) == Mod.CRC16;
}

bool SignatureMatcher::matchTail(const PatternModule &Mod, const uint8_t *Data,
                                 size_t Available) {
  if (Mod.TailBytes.empty())
    return true;
  size_t TailStart = Mod.LeadingBytes.size() + Mod.CRCLen;
  for (size_t I = 0; I < Mod.TailBytes.size(); ++I) {
    size_t Offset = TailStart + I;
    if (Offset >= Available)
      return false;
    if (Mod.TailBytes[I].IsWildcard)
      continue;
    if (Data[Offset] != Mod.TailBytes[I].Value)
      return false;
  }
  return true;
}

bool SignatureMatcher::matchPattern(const PatternModule &Mod,
                                    const uint8_t *Data, size_t Available) {
  if (Mod.TotalLen > 0 && Available < Mod.TotalLen)
    return false;
  if (!matchLeading(Mod.LeadingBytes, Data, Available))
    return false;
  if (!verifyCRC(Mod, Data, Available))
    return false;
  return matchTail(Mod, Data, Available);
}

void SignatureMatcher::scanRegion(const uint8_t *Data, size_t Size,
                                  const std::vector<PatternModule> &Modules,
                                  MatchCallback Callback) {
  HashIndex Idx;
  Idx.build(Modules);

  for (size_t Offset = 0; Offset < Size; ++Offset) {
    size_t Available = Size - Offset;
    uint8_t B0 = Data[Offset];
    uint8_t B1 = (Offset + 1 < Size) ? Data[Offset + 1] : 0;
    uint16_t Key = (static_cast<uint16_t>(B0) << 8) | B1;

    for (size_t ModIdx : Idx.Buckets[Key]) {
      if (matchPattern(Modules[ModIdx], Data + Offset, Available))
        Callback(Offset, Modules[ModIdx]);
    }
    for (size_t ModIdx : Idx.WildcardBucket) {
      if (matchPattern(Modules[ModIdx], Data + Offset, Available))
        Callback(Offset, Modules[ModIdx]);
    }
  }
}

void SignatureMatcher::HashIndex::build(
    const std::vector<PatternModule> &Modules) {
  Buckets.clear();
  Buckets.resize(kBuckets);
  WildcardBucket.clear();

  for (size_t I = 0; I < Modules.size(); ++I) {
    if (isWildcardKey(Modules[I])) {
      WildcardBucket.push_back(I);
    } else {
      Buckets[keyOf(Modules[I])].push_back(I);
    }
  }
}

uint16_t SignatureMatcher::HashIndex::keyOf(const PatternModule &Mod) const {
  return (static_cast<uint16_t>(Mod.LeadingBytes[0].Value) << 8) |
         Mod.LeadingBytes[1].Value;
}

bool SignatureMatcher::HashIndex::isWildcardKey(
    const PatternModule &Mod) const {
  return Mod.LeadingBytes.size() < 2 || Mod.LeadingBytes[0].IsWildcard ||
         Mod.LeadingBytes[1].IsWildcard;
}

void SignatureMatcher::scanAtAddresses(
    const uint8_t *ImageBase, size_t ImageSize, uint64_t BaseVA,
    const std::vector<uint64_t> &FuncEntries,
    const std::vector<PatternModule> &Modules, MatchCallback Callback) {
  HashIndex Idx;
  Idx.build(Modules);

  for (uint64_t Entry : FuncEntries) {
    if (Entry < BaseVA)
      continue;
    uint64_t Offset = Entry - BaseVA;
    if (Offset >= ImageSize)
      continue;
    size_t Available = ImageSize - static_cast<size_t>(Offset);

    uint8_t B0 = ImageBase[Offset];
    uint8_t B1 = (Offset + 1 < ImageSize) ? ImageBase[Offset + 1] : 0;
    uint16_t Key = (static_cast<uint16_t>(B0) << 8) | B1;

    for (size_t ModIdx : Idx.Buckets[Key]) {
      if (matchPattern(Modules[ModIdx], ImageBase + Offset, Available))
        Callback(Entry, Modules[ModIdx]);
    }
    for (size_t ModIdx : Idx.WildcardBucket) {
      if (matchPattern(Modules[ModIdx], ImageBase + Offset, Available))
        Callback(Entry, Modules[ModIdx]);
    }
  }
}
