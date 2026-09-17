#import <Foundation/Foundation.h>
#include <stdint.h>
#include <swift/shims/AssertionReporting.h>

@interface NDDiagnosticReports : NSObject
- (uint64_t)initializer;
- (uint64_t)initializerInFile;
- (uint64_t)fatal;
- (uint64_t)fatalInFile;
- (void)terminal;
- (void)terminalViaNative:(unsigned)value;
@end

__attribute__((noinline, noreturn)) static void
nativeReportAndTrap(unsigned value) {
  if (value)
    _swift_stdlib_reportUnimplementedInitializer(
        (const unsigned char *)"NativeTaken", 11, (const unsigned char *)"init",
        4, 0);
  else
    _swift_stdlib_reportUnimplementedInitializer(
        (const unsigned char *)"NativeOther", 11, (const unsigned char *)"init",
        4, 0);
  __builtin_trap();
}

@implementation NDDiagnosticReports
- (void)terminalViaNative:(unsigned)value {
  nativeReportAndTrap(value);
}
- (uint64_t)initializer {
  _swift_stdlib_reportUnimplementedInitializer(
      (const unsigned char *)"BoundedClassSuffix", 12,
      (const unsigned char *)"init:\0hidden", 12, 0);
  return 42;
}
- (uint64_t)initializerInFile {
  _swift_stdlib_reportUnimplementedInitializerInFile(
      (const unsigned char *)"BoundedClassSuffix" + 7, 5,
      (const unsigned char *)"init:", 5,
      (const unsigned char *)"source\"\\.swift", 14, 31, 17, 0);
  return 91;
}
- (uint64_t)fatal {
  _swift_stdlib_reportFatalError((const unsigned char *)"prefix!", 6,
                                 (const unsigned char *)"caf\xc3\xa9!", 5, 0);
  return 137;
}
- (uint64_t)fatalInFile {
  _swift_stdlib_reportFatalErrorInFile(
      (const unsigned char *)"prefix!", 6, (const unsigned char *)"message?", 7,
      (const unsigned char *)"source\"\\.swift", 14, 61, 0);
  return 251;
}
- (void)terminal {
  _swift_stdlib_reportUnimplementedInitializer(
      (const unsigned char *)"Terminal", 8, (const unsigned char *)"init", 4,
      0);
  __builtin_trap();
}
@end
