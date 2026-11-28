// Native probe for macos_sdk_load.py. Build with the macOS 13 or newer SDK.
#include <dlfcn.h>
#include <err.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libproc.h>
#include <mach-o/loader.h>
#include <mach-o/utils.h>
#include <mach/mach_time.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/proc_info.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct usage {
  uint64_t cpu;
  uint64_t runnable;
};

struct observer {
  pid_t pid;
  int request;
  int reply;
  struct usage previous;
  mach_timebase_info_data_t timebase;
};

static _Noreturn void observe(pid_t target, int request, int reply) {
  char event;
  ssize_t received;
  while ((received = read(request, &event, 1)) == 1) {
    // XNU closes the current runnable interval when the thread blocks.
    struct proc_taskinfo task;
    do {
      if (proc_pidinfo(target, PROC_PIDTASKINFO, 0, &task, sizeof(task)) !=
          sizeof(task))
        err(1, "proc_pidinfo");
      if (task.pti_threadnum != 1)
        errx(1, "the SDK probe needs a single-threaded process");
      if (task.pti_numrunning)
        sched_yield();
    } while (task.pti_numrunning);
    struct rusage_info_v4 info;
    if (proc_pid_rusage(target, RUSAGE_INFO_V4, (rusage_info_t *)&info))
      err(1, "proc_pid_rusage");
    struct usage sample = {info.ri_user_time + info.ri_system_time,
                           info.ri_runnable_time};
    if (write(reply, &sample, sizeof(sample)) != sizeof(sample))
      err(1, "send counters");
  }
  if (received < 0)
    err(1, "read observer request");
  _exit(0);
}

static struct usage sample(const struct observer *obs) {
  struct usage current;
  if (write(obs->request, "+", 1) != 1 ||
      read(obs->reply, &current, sizeof(current)) != sizeof(current))
    errx(1, "could not obtain counters from the observer");
  return current;
}

static struct observer start_observer(void) {
  int request[2], reply[2];
  if (pipe(request) || pipe(reply))
    err(1, "pipe");
  pid_t target = getpid();
  pid_t pid = fork();
  if (pid < 0)
    err(1, "fork");
  if (pid == 0) {
    close(request[1]);
    close(reply[0]);
    observe(target, request[0], reply[1]);
  }
  close(request[0]);
  close(reply[1]);
  struct observer obs = {.pid = pid, .request = request[1], .reply = reply[0]};
  mach_timebase_info(&obs.timebase);
  return obs;
}

static void finish(struct observer *obs, const char *phase, uint64_t start) {
  uint64_t wall = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start;
  struct usage current = sample(obs);
  double ns = (double)obs->timebase.numer / obs->timebase.denom;
  printf("phase %s %" PRIu64 " %.0f %.0f\n", phase, wall,
         (current.cpu - obs->previous.cpu) * ns,
         (current.runnable - obs->previous.runnable) * ns);
  obs->previous = current;
}

static void residency(int fd, size_t size) {
  size_t page_size = (size_t)getpagesize();
  size_t pages = (size - 1) / page_size + 1;
  void *mapping = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (mapping == MAP_FAILED)
    err(1, "mmap for mincore");
  char *states = malloc(pages);
  if (!states || mincore(mapping, size, states))
    err(1, "mincore");
  size_t resident = 0;
  for (size_t i = 0; i < pages; ++i)
    resident += (states[i] & MINCORE_INCORE) != 0;
  free(states);
  munmap(mapping, size);
  printf("residency %zu %zu\n", resident, pages);
}

static fsignatures_t signature_in_native_slice(int fd) {
  __block fsignatures_t signature = {0};
  int error = macho_best_slice_in_fd(fd, ^(const struct mach_header *header,
                                           uint64_t slice, size_t size) {
    size_t header_size = sizeof(struct mach_header_64);
    if (size < header_size || header->magic != MH_MAGIC_64 ||
        header->filetype != MH_DYLIB || header->sizeofcmds > size - header_size)
      return;
    const char *command = (const char *)header + header_size;
    size_t left = header->sizeofcmds;
    for (uint32_t i = 0; i < header->ncmds; ++i) {
      struct load_command lc;
      if (left < sizeof(lc))
        return;
      memcpy(&lc, command, sizeof(lc));
      if (lc.cmdsize < sizeof(lc) || lc.cmdsize > left)
        return;
      if (lc.cmd == LC_CODE_SIGNATURE) {
        struct linkedit_data_command data;
        if (lc.cmdsize < sizeof(data))
          return;
        memcpy(&data, command, sizeof(data));
        if (data.dataoff <= size && data.datasize <= size - data.dataoff) {
          signature.fs_file_start = (off_t)slice;
          signature.fs_blob_start = (void *)(uintptr_t)data.dataoff;
          signature.fs_blob_size = data.datasize;
        }
        return;
      }
      command += lc.cmdsize;
      left -= lc.cmdsize;
    }
  });
  if (error)
    errx(1, "select native Mach-O slice: %s", strerror(error));
  if (!signature.fs_blob_size)
    errx(1, "the native SDK slice needs an embedded code signature");
  return signature;
}

int main(int argc, char **argv) {
  if (argc != 3 || (strcmp(argv[1], "direct") && strcmp(argv[1], "staged")))
    errx(1, "usage: macos-sdk-load {direct|staged} /path/to/libneverd.dylib");
  const char *path = argv[2];
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    err(1, "open SDK");
  struct stat expected;
  if (fstat(fd, &expected))
    err(1, "stat SDK");
  if (!S_ISREG(expected.st_mode) || expected.st_size <= 0)
    errx(1, "expected a nonempty SDK file");
  struct observer obs = start_observer();
  residency(fd, (size_t)expected.st_size);
  obs.previous = sample(&obs);
  uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  finish(&obs, "empty", start);
  if (!strcmp(argv[1], "staged")) {
    start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    fsignatures_t signature = signature_in_native_slice(fd);
    finish(&obs, "inspect", start);
    fchecklv_t check = {.lv_file_start = signature.fs_file_start};
    start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (fcntl(fd, F_ADDFILESIGS_RETURN, &signature) == -1)
      err(1, "register embedded signature");
    finish(&obs, "register_signature", start);
    char message[512] = {0};
    check.lv_error_message = message;
    check.lv_error_message_size = sizeof(message);
    start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (fcntl(fd, F_CHECK_LV, &check) == -1)
      err(1, "library validation: %s", message);
    finish(&obs, "check_library", start);
  }
  start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  void *sdk = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!sdk)
    errx(1, "dlopen: %s", dlerror());
  finish(&obs, "dlopen", start);
  Dl_info image;
  struct stat actual;
  void *symbol = dlsym(sdk, "neverd_session_create");
  if (!symbol || !dladdr(symbol, &image) || stat(image.dli_fname, &actual) ||
      actual.st_dev != expected.st_dev || actual.st_ino != expected.st_ino)
    errx(1, "the SDK entry point did not come from the measured file");
  printf("image %" PRIuMAX " %" PRIuMAX "\n", (uintmax_t)actual.st_dev,
         (uintmax_t)actual.st_ino);
  close(fd);
  close(obs.request);
  close(obs.reply);
  int status;
  if (waitpid(obs.pid, &status, 0) < 0 || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0)
    errx(1, "observer failed");
  return 0;
}
