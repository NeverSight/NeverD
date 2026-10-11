//===- DarwinSystemTestData.h - Explicit sysctl observations ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TESTS_DARWINSYSTEMTESTDATA_H
#define NEVERD_TESTS_DARWINSYSTEMTESTDATA_H

#include "neverd/emulation/DarwinSystemOptions.h"

namespace neverd::emulation::darwin_test {
inline DarwinSystemOptions systemOptions() {
  DarwinSystemOptions O;
  O.OSType = "Darwin";
  O.OSRelease = "24.test";
  O.OSRevision = INT32_MIN;
  O.OSVersion = "V42";
  O.KernelVersion = "NeverD virtual kernel";
  O.Machine = "virtual64";
  O.Model = "VirtualModel";
  O.CPUCount = 7;
  O.MemorySize = 0xfedcba9876543210ULL;
  return O;
}
inline constexpr char SystemJSON[] = R"({
  "os_type":"Darwin","os_release":"24.test","os_revision":-2147483648,
  "os_version":"V42","kernel_version":"NeverD virtual kernel",
  "machine":"virtual64","model":"VirtualModel","cpu_count":7,
  "memory_size":"18364758544493064720"})";
// Independent expected concatenation in native MIB order, including NULs.
inline constexpr char SystemHex[] =
    "44617277696e0032342e746573740000000080"
    "4e6576657244207669727475616c206b65726e656c0056343200"
    "7669727475616c3634005669727475616c4d6f64656c0007000000"
    "1032547698badcfe";
inline DarwinSystemOptions threadIdentityOptions() {
  DarwinSystemOptions O;
  O.ThreadID = 0xfedcba9876543210ULL;
  return O;
}
inline constexpr char ThreadIdentityJSON[] =
    R"({"thread_id":"18364758544493064720"})";
// Independent little-endian complete uint64 observation.
inline constexpr char ThreadIdentityHex[] = "1032547698badcfe";
inline DarwinSystemOptions hostNameOptions() {
  DarwinSystemOptions O;
  O.HostName = "abcd";
  return O;
}
inline constexpr char HostNameJSON[] = R"({"hostname":"abcd"})";
inline constexpr char HostNameHex[] = "6162636400";
inline DarwinSystemOptions processObservationOptions() {
  DarwinSystemOptions O;
  O.ProcessGroupID = 7;
  O.SessionID = 0x01020304;
  O.ProcessTainted = true;
  return O;
}
inline constexpr char ProcessObservationsJSON[] =
    R"({"process_group_id":7,"session_id":16909060,"process_tainted":true})";
// Independently packed little-endian group, session and int taint result.
inline constexpr char ProcessObservationsHex[] = "070000000403020101000000";
inline DarwinSystemOptions priorityOptions() {
  DarwinSystemOptions O;
  O.ProcessNice = -7;
  return O;
}
inline constexpr char PriorityJSON[] = R"({"nice":-7})";
// Independent complete raw signed return bytes, rather than Linux 20-nice.
inline constexpr char PriorityHex[] = "f9ffffffffffffff";
inline DarwinSystemOptions loginBufferOptions() {
  DarwinSystemOptions O;
  auto &Bytes = O.LoginNameBytes.emplace(255, 0xa5);
  Bytes[0] = 0x4c;
  Bytes[1] = 0;
  Bytes[2] = 0xff;
  Bytes.back() = 0x7e;
  return O;
}
// Independently declared wire bytes for L, embedded NUL, 0xff, nonzero tail.
inline constexpr char LoginNameHex[] =
    "4c00ffa5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5"
    "a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5"
    "a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5"
    "a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5"
    "a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5"
    "a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5"
    "a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5"
    "a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a57e";
static_assert(sizeof(LoginNameHex) == 511);
inline const std::string LoginNameJSON =
    std::string(R"({"login_name_hex":")") + LoginNameHex + "\"}";
inline DarwinSystemOptions resourceLimitOptions() {
  DarwinSystemOptions O;
  O.MaxFilesPerProcess = 64;
  O.ResourceLimits = {{0, {0ULL, 0ULL}},
                      {1, {9223372036854775807ULL, 9223372036854775807ULL}},
                      {2, {81985529216486895ULL, 9223372036854775807ULL}},
                      {3, {1073741824ULL, 2147483648ULL}},
                      {4, {0ULL, 9223372036854775807ULL}},
                      {5, {9223372036854775806ULL, 9223372036854775807ULL}},
                      {6, {8192ULL, 16384ULL}},
                      {7, {32ULL, 128ULL}},
                      {8, {256ULL, 1024ULL}}};
  return O;
}
inline constexpr char ResourceLimitsJSON[] =
    R"({"max_files_per_process":64,"resource_limits":[{"resource":0,"current":0,"maximum":0},{"resource":1,"current":"9223372036854775807","maximum":"9223372036854775807"},{"resource":2,"current":"81985529216486895","maximum":"9223372036854775807"},{"resource":3,"current":1073741824,"maximum":2147483648},{"resource":4,"current":0,"maximum":"9223372036854775807"},{"resource":5,"current":"9223372036854775806","maximum":"9223372036854775807"},{"resource":6,"current":8192,"maximum":16384},{"resource":7,"current":32,"maximum":128},{"resource":8,"current":256,"maximum":1024}]})";
// Independently packed expected pairs in canonical resource order.
inline constexpr char ResourceLimitsHex[] =
    "00000000000000000000000000000000ffffffffffffff7fffffffffffffff7f"
    "efcdab8967452301ffffffffffffff7f00000040000000000000008000000000"
    "0000000000000000ffffffffffffff7ffeffffffffffff7fffffffffffffff7f"
    "0020000000000000004000000000000020000000000000008000000000000000"
    "00010000000000000004000000000000";
inline DarwinSystemOptions resourceUsageOptions() {
  DarwinSystemOptions O;
  O.ResourceUsageSelf = DarwinResourceUsage{
      INT64_MIN,
      999999,
      81985529216486895LL,
      123456,
      {INT64_MAX, INT64_MIN, -1LL, 0LL, 1LL, 81985529216486895LL,
       -81985529216486895LL, 72623859790382856LL, 9LL, -10LL, 11LL, -12LL, 13LL,
       -14LL}};
  O.ResourceUsageChildren =
      DarwinResourceUsage{INT64_MAX,
                          0,
                          -3LL,
                          1,
                          {0LL, -1LL, 2LL, -3LL, 4LL, -5LL, 6LL, -7LL, 8LL,
                           -9LL, 10LL, -11LL, 12LL, INT64_MAX}};
  return O;
}
inline constexpr char ResourceUsageJSON[] =
    R"({"resource_usage":{"self":{"user_seconds":"-9223372036854775808","user_microseconds":999999,"system_seconds":"81985529216486895","system_microseconds":123456,"counters":["9223372036854775807","-9223372036854775808",-1,0,1,"81985529216486895","-81985529216486895","72623859790382856",9,-10,11,-12,13,-14]},"children":{"user_seconds":"9223372036854775807","user_microseconds":0,"system_seconds":-3,"system_microseconds":1,"counters":[0,-1,2,-3,4,-5,6,-7,8,-9,10,-11,12,"9223372036854775807"]}}})";
// Independently packed signed timevals, zero padding and all fourteen longs.
inline constexpr char ResourceUsageHex[] =
    "00000000000000803f420f0000000000efcdab896745230140e2010000000000"
    "ffffffffffffff7f0000000000000080ffffffffffffffff0000000000000000"
    "0100000000000000efcdab89674523011132547698badcfe0807060504030201"
    "0900000000000000f6ffffffffffffff0b00000000000000f4ffffffffffffff"
    "0d00000000000000f2ffffffffffffffffffffffffffff7f0000000000000000"
    "fdffffffffffffff01000000000000000000000000000000ffffffffffffffff"
    "0200000000000000fdffffffffffffff0400000000000000fbffffffffffffff"
    "0600000000000000f9ffffffffffffff0800000000000000f7ffffffffffffff"
    "0a00000000000000f5ffffffffffffff0c00000000000000ffffffffffffff7f";
inline DarwinSystemOptions credentialOptions() {
  DarwinSystemOptions O;
  O.Credentials = DarwinCredentials{
      101, 202, 303, 404, std::vector<uint32_t>{404, 0, INT32_MAX, 7, 7}};
  return O;
}
inline constexpr char CredentialsJSON[] =
    R"({"credentials":{"real_uid":101,"effective_uid":202,"real_gid":303,"effective_gid":404,"groups":[404,0,"2147483647",7,7]}})";
// Independently packed little-endian gids and four scalar IDs/count/list.
inline constexpr char GroupsHex[] = "9401000000000000ffffff7f0700000007000000";
inline constexpr char CredentialsHex[] =
    "65000000ca0000002f01000094010000050000009401000000000000ffffff7f0700000007"
    "000000";
inline DarwinSystemOptions machSelfPortOptions() {
  DarwinSystemOptions O;
  O.ThreadSelfPort = 0x80000001;
  O.TaskSelfPort = 0;
  O.HostSelfPort = UINT32_MAX;
  return O;
}
inline constexpr char MachSelfPortsJSON[] =
    R"({"thread_self_port":2147483649,"task_self_port":0,"host_self_port":"4294967295"})";
// Independent little-endian signed raw64 thread/task/host return carriers.
inline constexpr char MachSelfPortsHex[] =
    "01000080ffffffff0000000000000000ffffffffffffffff";
} // namespace neverd::emulation::darwin_test
#endif
