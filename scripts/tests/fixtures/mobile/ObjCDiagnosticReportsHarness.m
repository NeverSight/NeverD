#import <Foundation/Foundation.h>
#include <limits.h>
#import <objc/runtime.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

@interface NDDiagnosticReports : NSObject
- (uint64_t)initializer;
- (uint64_t)initializerInFile;
- (uint64_t)fatal;
- (uint64_t)fatalInFile;
- (void)terminal;
- (void)terminalViaNative:(unsigned)value;
@end

#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

// Observe the actual instruction fault without invoking the host's crash
// reporter. Default signal termination may stall while a report is processed,
// even with core files disabled. The pipe receipt distinguishes a real signal
// from a method that simply exits with the same status. Both operations in
// the handler are async-signal-safe.
static volatile sig_atomic_t trapSignalFD = -1;
static void observedTrap(int signal) {
  const unsigned char receipt = (unsigned char)signal;
  if (write(trapSignalFD, &receipt, 1) != 1)
    _exit(10);
  _exit(128 + signal);
}

int main(int argc, char **argv) {
  struct rlimit core = {0, 0};
  if (setrlimit(RLIMIT_CORE, &core))
    return 2;
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDDiagnosticReports *object = [NDDiagnosticReports new];
    if (argc == 4 && !strcmp(argv[1], "--trap")) {
      char *end = NULL;
      long fd = strtol(argv[2], &end, 10);
      if (end == argv[2] || *end || fd < 0 || fd > INT_MAX)
        return 9;
      trapSignalFD = (int)fd;
      struct sigaction action = {0};
      action.sa_handler = observedTrap;
      sigemptyset(&action.sa_mask);
      if (sigaction(SIGTRAP, &action, NULL) || sigaction(SIGILL, &action, NULL))
        return 9;
      if (!strcmp(argv[3], "0"))
        [object terminal];
      else
        [object terminalViaNative:!strcmp(argv[3], "1") ? 17 : 0];
      return 3;
    }
    if ([object initializer] != 42 || [object initializerInFile] != 91 ||
        [object fatal] != 137 || [object fatalInFile] != 251)
      return 4;
    [object release];
    for (unsigned mode = 0; mode < 3; ++mode) {
      int receiptPipe[2];
      if (pipe(receiptPipe))
        return 10;
      char descriptor[32];
      snprintf(descriptor, sizeof(descriptor), "%d", receiptPipe[1]);
      pid_t pid = fork();
      if (pid < 0)
        return 5;
      if (!pid) {
        close(receiptPipe[0]);
        char choice[2] = {(char)('0' + mode), 0};
        execl(argv[0], argv[0], "--trap", descriptor, choice, NULL);
        _exit(6);
      }
      close(receiptPipe[1]);
      int status;
      if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        return 7;
      unsigned char receipt = 0;
      const ssize_t received = read(receiptPipe[0], &receipt, 1);
      close(receiptPipe[0]);
#if defined(__arm64__)
      if (received != 1 || receipt != SIGTRAP ||
          WEXITSTATUS(status) != 128 + SIGTRAP)
        return 8;
#else
      if (received != 1 || receipt != SIGILL ||
          WEXITSTATUS(status) != 128 + SIGILL)
        return 8;
#endif
    }
    puts("diagnostic-runtime=pass\ncontents=pass\ntrap=pass\nnative-traps=2");
  }
}
