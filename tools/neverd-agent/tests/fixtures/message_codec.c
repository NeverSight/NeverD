/* Local, non-executed PE fixture with independently checkable semantics. */
typedef unsigned char byte;
typedef unsigned int uint;
__declspec(dllexport) uint PacketChecksum(const byte *data, uint length) {
  uint value = 0;
  for (uint i = 0; i < length; ++i)
    value = (value + data[i]) & 0xffff;
  return value;
}
__declspec(dllexport) int ValidatePacket(const byte *data, uint length) {
  if (!data || length < 8)
    return -1;
  if (data[0] != 'N' || data[1] != 'D')
    return -2;
  if (data[2] != 1 && data[2] != 2)
    return -3;
  uint payload = data[4] | ((uint)data[5] << 8);
  if (payload != length - 8)
    return -4;
  uint expected = data[6] | ((uint)data[7] << 8);
  if (PacketChecksum(data + 8, payload) != expected)
    return -5;
  return data[2];
}
