//===-- macos-warm-launch.c - Time a warm CLI launch --------------------===//
//
// SPDX-License-Identifier: AGPL-3.0-only
//
//===----------------------------------------------------------------------===//

#include <errno.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <unistd.h>

#if defined(NEVERD_WARM_ENTRY)

// The driver's pipe is empty until after the original main returns.
extern int main(int, char **);
int neverd_warm_entry(int argc, char **argv) {
  uint64_t marks[2];
  marks[0] = mach_absolute_time();
  int result = main(argc, argv);
  marks[1] = mach_absolute_time();
  ssize_t written;
  do {
    written = write(3, marks, sizeof(marks));
  } while (written < 0 && errno == EINTR);
  return written == (ssize_t)sizeof(marks) ? result : 125;
}

#else

#include <libproc.h>
#include <mach/mach.h>
#include <sys/resource.h>
#include <sys/wait.h>

uint64_t warm_clock(void) { return mach_absolute_time(); }

double warm_ms_per_tick(void) {
  mach_timebase_info_data_t timebase;
  mach_timebase_info(&timebase);
  return (double)timebase.numer / timebase.denom / 1000000.0;
}

int warm_host_ticks(uint32_t ticks[CPU_STATE_MAX]) {
  host_cpu_load_info_data_t load;
  mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
  mach_port_t host = mach_host_self();
  kern_return_t status = host_statistics(
      host, HOST_CPU_LOAD_INFO, (host_info_t)&load, &count);
  mach_port_deallocate(mach_task_self(), host);
  if (status != KERN_SUCCESS)
    return -1;
  for (unsigned i = 0; i < CPU_STATE_MAX; ++i)
    ticks[i] = load.cpu_ticks[i];
  return 0;
}

// Keep the zombie until the caller reaps it. The exit timestamp then
// separates child teardown from any delay waking the parent.
int warm_wait_exit(int pid, uint64_t record[5]) {
  siginfo_t child;
  int result;
  do {
    result = waitid(P_PID, (id_t)pid, &child, WEXITED | WNOWAIT);
  } while (result < 0 && errno == EINTR);
  if (result < 0)
    return -1;
  record[4] = mach_absolute_time();
  struct rusage_info_v4 info;
  if (proc_pid_rusage(pid, RUSAGE_INFO_V4, (rusage_info_t *)&info) < 0)
    return -1;
  record[0] = info.ri_proc_start_abstime;
  record[1] = info.ri_proc_exit_abstime;
  record[2] = info.ri_user_time + info.ri_system_time;
  record[3] = info.ri_instructions;
  return 0;
}

#endif
