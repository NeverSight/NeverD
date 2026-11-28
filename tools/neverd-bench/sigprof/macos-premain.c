//===-- macos-premain.c - Account for time before main on macOS -----------===//
//
// SPDX-License-Identifier: AGPL-3.0-only
//
//===----------------------------------------------------------------------===//

#include <errno.h>
#include <libproc.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <sys/proc_info.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#if defined(NEVERD_PREMAIN_ENTRY)
extern int main(int, char **);

int neverd_premain_entry(int argc, char **argv) {
  uint64_t marks[6];
  marks[0] = mach_absolute_time();
  pid_t pid = getpid();
  struct rusage_info_v4 info;
  if (proc_pid_rusage(pid, RUSAGE_INFO_V4, (rusage_info_t *)&info))
    return 125;
  marks[1] = info.ri_user_time + info.ri_system_time;
  marks[2] = info.ri_runnable_time;
  marks[3] = mach_absolute_time();
  char token = '+';
  ssize_t written;
  do {
    written = write(3, &token, 1);
  } while (written < 0 && errno == EINTR);
  if (written != 1)
    return 125;
  ssize_t received;
  do {
    received = read(3, &token, 1);
  } while (received < 0 && errno == EINTR);
  if (received != 1 || token != '+')
    return 125;
  marks[4] = mach_absolute_time();
  int status = main(argc, argv);
  marks[5] = mach_absolute_time();
  do {
    written = write(3, marks, sizeof(marks));
  } while (written < 0 && errno == EINTR);
  return written == (ssize_t)sizeof(marks) ? status : 125;
}
#else
struct entry_usage {
  uint64_t observed, birth, cpu, runnable, instructions, pageins, bytes_read;
  uint64_t faults, cow_faults, threads;
};

// The parent acknowledges the entry byte after sampling. A blocked initial
// thread has closed its runnable interval and can accrue no observer delay.
int premain_snapshot(pid_t pid, struct entry_usage *result) {
  const struct timespec interval = {.tv_nsec = 1000000};
  uint64_t deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 120000000000ULL;
  struct rusage_info_v4 info;
  struct proc_taskinfo task;
  for (;;) {
    if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &task, sizeof(task)) !=
        sizeof(task))
      return -1;
    if (task.pti_numrunning == 0 || task.pti_threadnum != 1)
      break;
    if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) >= deadline) {
      errno = ETIMEDOUT;
      return -1;
    }
    nanosleep(&interval, NULL);
  }
  // A joined initializer thread still contributes to task-wide CPU and
  // runnable time. A single surviving thread is insufficient on its own.
  if (task.pti_total_user != task.pti_threads_user ||
      task.pti_total_system != task.pti_threads_system) {
    errno = ENOTSUP;
    return -1;
  }
  result->observed = mach_absolute_time();
  if (proc_pid_rusage(pid, RUSAGE_INFO_V4, (rusage_info_t *)&info))
    return -1;
  result->birth = info.ri_proc_start_abstime;
  result->cpu = info.ri_user_time + info.ri_system_time;
  result->runnable = info.ri_runnable_time;
  result->instructions = info.ri_instructions;
  result->pageins = info.ri_pageins;
  result->bytes_read = info.ri_diskio_bytesread;
  result->faults = task.pti_faults;
  result->cow_faults = task.pti_cow_faults;
  result->threads = task.pti_threadnum;
  return 0;
}
#endif
