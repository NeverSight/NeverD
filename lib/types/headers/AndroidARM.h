// The Android types and prototypes for ARM (ILP32), as a program declares them
// with bionic's headers, <jni.h>, <android/log.h>, <sys/system_properties.h>
// and _GNU_SOURCE, for API level 35.  Generated on 2026-11-07 by
// scripts/generate_linux_types.py --target android-arm from the headers of
// bionic at commit 731631f300090436d7f5df80d50b6275c8c60a93, libnativehelper at
// commit aef2939781fc0b57b4477df7160935cdf5697919 and system/logging at commit
// bcac7c30d88a3773a7c0bc9f5617a23a886331fd: changed from them, as NeverD reads
// and renders their declarations again.  The headers' notices are in
// LICENSES/bionic-headers.txt and LICENSES/android-ndk-headers.txt.  Do not
// edit; generate it again.
// 528 types, 1416 prototypes, rendered by NeverD.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef DECLSPEC_ALIGN
#if defined(_MSC_VER)
#define DECLSPEC_ALIGN(x) __declspec(align(x))
#else
#define DECLSPEC_ALIGN(x) __attribute__((aligned(x)))
#endif
#endif

#ifndef NEVERD_ALIGN8
#if !defined(_MSC_VER) && defined(__i386__)
#define NEVERD_ALIGN8 __attribute__((aligned(8)))
#else
#define NEVERD_ALIGN8
#endif
#endif

#ifndef NEVERD_GNU_STRUCT
#if defined(__MINGW32__) && (defined(__i386__) || defined(__x86_64__))
#define NEVERD_GNU_STRUCT __attribute__((gcc_struct))
#else
#define NEVERD_GNU_STRUCT
#endif
#endif

#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif

static_assert(sizeof(long) == 4, "these types need a 32-bit long");

typedef struct __kernel_fd_set __kernel_fd_set;
typedef struct __kernel_fsid_t __kernel_fsid_t;
typedef struct pthread_attr_t pthread_attr_t;
typedef struct pthread_barrier_t pthread_barrier_t;
typedef struct pthread_cond_t pthread_cond_t;
typedef struct pthread_mutex_t pthread_mutex_t;
typedef struct pthread_rwlock_t pthread_rwlock_t;
typedef struct pthread_spinlock_t pthread_spinlock_t;
struct __sFILE;
struct mallinfo;
struct mallinfo2;
struct __locale_t;
typedef struct div_t div_t;
typedef struct ldiv_t ldiv_t;
typedef struct lldiv_t lldiv_t;
struct timespec;
struct __kernel_timespec;
struct __kernel_itimerspec;
struct __kernel_old_timespec;
struct __kernel_sock_timeval;
struct timeval;
struct itimerspec;
struct itimerval;
struct timezone;
struct sigcontext;
struct siginfo;
struct __kernel_sigaction;
struct sigaltstack;
union sigval;
union __sifields;
struct sigevent;
typedef struct sigset64_t sigset64_t;
struct sigaction;
struct sigaction64;
struct fp_reg;
struct user_fpregs;
struct user_regs;
struct user_vfp;
struct user_vfp_exc;
struct user;
struct ucontext;
typedef struct fd_set fd_set;
struct flock;
struct flock64;
struct f_owner_ex;
struct open_how;
struct statx_timestamp;
struct statx;
struct iovec;
struct dmabuf_cmsg;
struct dmabuf_token;
struct __timezone_t;
struct tm;
typedef struct mbstate_t mbstate_t;
struct lconv;
typedef struct imaxdiv_t imaxdiv_t;
struct dirent;
struct dirent64;
typedef struct DIR DIR;
typedef struct Dl_info Dl_info;
struct clone_args;
struct sched_attr;
struct sched_param;
typedef struct cpu_set_t cpu_set_t;
typedef struct __pthread_cleanup_t __pthread_cleanup_t;
typedef struct sem_t sem_t;
struct pollfd;
struct termios;
struct termios2;
struct ktermios;
struct winsize;
struct termio;
struct passwd;
struct group;
struct stat;
typedef struct glob_t glob_t;
struct re_guts;
typedef struct regex_t regex_t;
typedef struct regmatch_t regmatch_t;
struct option;
struct __posix_spawnattr;
struct __posix_spawn_file_actions;
struct __iconv_t;
typedef struct Elf32_Dyn Elf32_Dyn;
typedef struct Elf64_Dyn Elf64_Dyn;
struct elf32_rel;
struct elf64_rel;
struct elf32_rela;
struct elf64_rela;
struct elf32_sym;
struct elf64_sym;
struct elf32_hdr;
struct elf64_hdr;
struct elf32_phdr;
struct elf64_phdr;
struct elf32_shdr;
struct elf64_shdr;
struct elf32_note;
struct elf64_note;
typedef struct Elf32_auxv_t Elf32_auxv_t;
typedef struct Elf64_auxv_t Elf64_auxv_t;
typedef struct Elf32_Chdr Elf32_Chdr;
typedef struct Elf64_Chdr Elf64_Chdr;
typedef struct Elf32_Lib Elf32_Lib;
typedef struct Elf64_Lib Elf64_Lib;
typedef struct Elf32_Move Elf32_Move;
typedef struct Elf64_Move Elf64_Move;
typedef struct Elf32_Syminfo Elf32_Syminfo;
typedef struct Elf64_Syminfo Elf64_Syminfo;
typedef struct Elf32_Verdef Elf32_Verdef;
typedef struct Elf32_Verdaux Elf32_Verdaux;
typedef struct Elf64_Verdef Elf64_Verdef;
typedef struct Elf64_Verdaux Elf64_Verdaux;
typedef struct Elf32_Verneed Elf32_Verneed;
typedef struct Elf32_Vernaux Elf32_Vernaux;
typedef struct Elf64_Verneed Elf64_Verneed;
typedef struct Elf64_Vernaux Elf64_Vernaux;
struct dl_phdr_info;
struct link_map;
struct r_debug;
struct sockaddr_storage;
struct sockaddr;
struct linger;
struct msghdr;
struct mmsghdr;
struct cmsghdr;
struct ucred;
struct hostent;
struct netent;
struct servent;
struct protoent;
struct addrinfo;
struct in6_addr;
struct sockaddr_in6;
struct ipv6_mreq;
struct in6_flowlabel_req;
struct in_addr;
struct ip_msfilter;
struct ip_mreq_source;
struct ip_mreq;
struct ip_mreqn;
struct group_req;
struct group_source_req;
struct group_filter;
struct in_pktinfo;
struct sockaddr_in;
struct in6_pktinfo;
struct ip6_mtuinfo;
struct in6_ifreq;
struct ipv6_rt_hdr;
struct ipv6_opt_hdr;
struct rt0_hdr;
struct rt2_hdr;
struct ipv6_destopt_hao;
struct ipv6hdr;
struct ifaddrs;
struct entry;
struct __hsearch;
struct hsearch_data;
struct utimbuf;
struct lastlog;
struct exit_status;
struct utmp;
struct stat64;
struct statvfs;
struct statvfs64;
struct tms;
struct rusage;
struct rlimit;
struct rlimit64;
struct cachestat_range;
struct cachestat;
struct sockaddr_un;
union epoll_data;
struct epoll_event;
struct epoll_params;
struct inotify_event;
struct utsname;
struct prctl_mm_map;
struct ptrace_peeksiginfo_args;
struct seccomp_metadata;
struct ptrace_syscall_info;
struct ptrace_rseq_configuration;
struct ptrace_sud_config;
struct pt_regs;
struct sysinfo;
struct signalfd_siginfo;
struct rand_pool_info;
struct vgetrandom_opaque_params;
struct __kernel_legacy_ipc_perm;
struct ipc64_perm;
struct msgbuf;
struct ipc_kludge;
struct sem;
struct sem_queue;
struct sem_undo;
struct __kernel_legacy_semid_ds;
struct semid64_ds;
struct sembuf;
struct seminfo;
union __kernel_legacy_semun;
union semun;
struct __kernel_legacy_shmid_ds;
struct shmid64_ds;
struct shminfo64;
struct shminfo;
struct shm_info;
struct msg;
struct __kernel_legacy_msqid_ds;
struct msqid64_ds;
struct msginfo;
struct tcphdr;
union tcp_word_hdr;
struct tcp_repair_opt;
struct tcp_repair_window;
struct tcp_info;
struct tcp_md5sig;
struct tcp_diag_md5sig;
struct tcp_ao_add;
struct tcp_ao_del;
struct tcp_ao_info_opt;
struct tcp_ao_getsockopt;
struct tcp_ao_repair;
struct tcp_zerocopy_receive;
typedef struct sync_serial_settings sync_serial_settings;
typedef struct te1_settings te1_settings;
typedef struct raw_hdlc_proto raw_hdlc_proto;
typedef struct fr_proto fr_proto;
typedef struct fr_proto_pvc fr_proto_pvc;
typedef struct fr_proto_pvc_info fr_proto_pvc_info;
typedef struct cisco_proto cisco_proto;
typedef struct x25_hdlc_proto x25_hdlc_proto;
struct ifmap;
struct if_settings;
struct ifreq;
struct ifconf;
struct if_nameindex;
struct _ftsent;
typedef struct FTS FTS;
struct FTW;
struct _jfieldID;
struct _jmethodID;
struct JNIInvokeInterface;
typedef union jvalue jvalue;
typedef struct JNINativeMethod JNINativeMethod;
struct _JNIEnv;
struct _JavaVM;
struct JNINativeInterface;
typedef struct JavaVMAttachArgs JavaVMAttachArgs;
typedef struct JavaVMOption JavaVMOption;
typedef struct JavaVMInitArgs JavaVMInitArgs;
struct __android_log_message;
typedef struct prop_info prop_info;

#pragma neverd_header("stdlib.h")
// HeapTaggingLevel: 4 bytes
enum HeapTaggingLevel
{
    M_HEAP_TAGGING_LEVEL_NONE = 0,
    M_HEAP_TAGGING_LEVEL_TBI = 1,
    M_HEAP_TAGGING_LEVEL_ASYNC = 2,
    M_HEAP_TAGGING_LEVEL_SYNC = 3,
};

#pragma neverd_header("search.h")
// ACTION: 4 bytes
enum ACTION
{
    FIND = 0,
    ENTER = 1,
};
typedef enum ACTION ACTION;

// VISIT: 4 bytes
enum VISIT
{
    preorder = 0,
    postorder = 1,
    endorder = 2,
    leaf = 3,
};
typedef enum VISIT VISIT;

#pragma neverd_header("netinet/tcp.h")
// tcp_fastopen_client_fail: 4 bytes
enum tcp_fastopen_client_fail
{
    TFO_STATUS_UNSPEC = 0,
    TFO_COOKIE_UNAVAILABLE = 1,
    TFO_DATA_NOT_ACKED = 2,
    TFO_SYN_RETRANSMITTED = 3,
};

// tcp_ca_state: 4 bytes
enum tcp_ca_state
{
    TCP_CA_Open = 0,
    TCP_CA_Disorder = 1,
    TCP_CA_CWR = 2,
    TCP_CA_Recovery = 3,
    TCP_CA_Loss = 4,
};

#pragma neverd_header("net/if.h")
// net_device_flags: 4 bytes
enum net_device_flags
{
    IFF_UP = 0x1,
    IFF_BROADCAST = 0x2,
    IFF_DEBUG = 0x4,
    IFF_LOOPBACK = 0x8,
    IFF_POINTOPOINT = 0x10,
    IFF_NOTRAILERS = 0x20,
    IFF_RUNNING = 0x40,
    IFF_NOARP = 0x80,
    IFF_PROMISC = 0x100,
    IFF_ALLMULTI = 0x200,
    IFF_MASTER = 0x400,
    IFF_SLAVE = 0x800,
    IFF_MULTICAST = 0x1000,
    IFF_PORTSEL = 0x2000,
    IFF_AUTOMEDIA = 0x4000,
    IFF_DYNAMIC = 0x8000,
    IFF_LOWER_UP = 0x10000,
    IFF_DORMANT = 0x20000,
    IFF_ECHO = 0x40000,
};

#pragma neverd_header("jni.h")
// jobjectRefType: 4 bytes
enum jobjectRefType
{
    JNIInvalidRefType = 0,
    JNILocalRefType = 1,
    JNIGlobalRefType = 2,
    JNIWeakGlobalRefType = 3,
};
typedef enum jobjectRefType jobjectRefType;

#pragma neverd_header("android/log.h")
// android_LogPriority: 4 bytes
enum android_LogPriority
{
    ANDROID_LOG_UNKNOWN = 0,
    ANDROID_LOG_DEFAULT = 1,
    ANDROID_LOG_VERBOSE = 2,
    ANDROID_LOG_DEBUG = 3,
    ANDROID_LOG_INFO = 4,
    ANDROID_LOG_WARN = 5,
    ANDROID_LOG_ERROR = 6,
    ANDROID_LOG_FATAL = 7,
    ANDROID_LOG_SILENT = 8,
};
typedef enum android_LogPriority android_LogPriority;

// log_id: 4 bytes
enum log_id
{
    LOG_ID_MIN = 0x0,
    LOG_ID_MAIN = 0x0,
    LOG_ID_RADIO = 0x1,
    LOG_ID_EVENTS = 0x2,
    LOG_ID_SYSTEM = 0x3,
    LOG_ID_CRASH = 0x4,
    LOG_ID_STATS = 0x5,
    LOG_ID_SECURITY = 0x6,
    LOG_ID_KERNEL = 0x7,
    LOG_ID_MAX = 0x8,
    LOG_ID_DEFAULT = 0x7FFFFFFF,
};

typedef unsigned int size_t;

typedef int ptrdiff_t;

#pragma neverd_header("stdio.h")
typedef signed char __int8_t;

typedef unsigned char __uint8_t;

typedef short __int16_t;

typedef unsigned short __uint16_t;

typedef int __int32_t;

typedef unsigned int __uint32_t;

typedef long long __int64_t;

typedef unsigned long long __uint64_t;

typedef int __intptr_t;

typedef unsigned int __uintptr_t;

typedef __intptr_t intptr_t;

typedef __uintptr_t uintptr_t;

typedef signed char int_least8_t;

typedef unsigned char uint_least8_t;

typedef short int_least16_t;

typedef unsigned short uint_least16_t;

typedef int int_least32_t;

typedef unsigned int uint_least32_t;

typedef long long int_least64_t;

typedef unsigned long long uint_least64_t;

typedef signed char int_fast8_t;

typedef unsigned char uint_fast8_t;

typedef long long int_fast64_t;

typedef unsigned long long uint_fast64_t;

typedef int int_fast16_t;

typedef unsigned int uint_fast16_t;

typedef int int_fast32_t;

typedef unsigned int uint_fast32_t;

typedef unsigned long long uintmax_t;

typedef long long intmax_t;

typedef signed char __s8;

typedef unsigned char __u8;

typedef short __s16;

typedef unsigned short __u16;

typedef int __s32;

typedef unsigned int __u32;

typedef long long __s64;

typedef unsigned long long __u64;

// __kernel_fd_set: 0x80 (128) bytes
struct __kernel_fd_set
{
    unsigned long fds_bits[32];
};

typedef void (*__kernel_sighandler_t)(int);

typedef int __kernel_key_t;

typedef int __kernel_mqd_t;

typedef unsigned short __kernel_mode_t;

typedef unsigned short __kernel_ipc_pid_t;

typedef unsigned short __kernel_uid_t;

typedef unsigned short __kernel_gid_t;

typedef unsigned short __kernel_old_dev_t;

typedef long __kernel_long_t;

typedef unsigned long __kernel_ulong_t;

typedef __kernel_ulong_t __kernel_ino_t;

typedef int __kernel_pid_t;

typedef __kernel_long_t __kernel_suseconds_t;

typedef int __kernel_daddr_t;

typedef unsigned int __kernel_uid32_t;

typedef unsigned int __kernel_gid32_t;

typedef __kernel_uid_t __kernel_old_uid_t;

typedef __kernel_gid_t __kernel_old_gid_t;

typedef unsigned int __kernel_size_t;

typedef int __kernel_ssize_t;

typedef int __kernel_ptrdiff_t;

// __kernel_fsid_t: 0x8 (8) bytes
struct __kernel_fsid_t
{
    int val[2];
};

typedef __kernel_long_t __kernel_off_t;

typedef long long __kernel_loff_t;

typedef __kernel_long_t __kernel_old_time_t;

typedef __kernel_long_t __kernel_time_t;

typedef long long __kernel_time64_t;

typedef __kernel_long_t __kernel_clock_t;

typedef int __kernel_timer_t;

typedef int __kernel_clockid_t;

typedef char *__kernel_caddr_t;

typedef unsigned short __kernel_uid16_t;

typedef unsigned short __kernel_gid16_t;

typedef __u16 __le16;

typedef __u16 __be16;

typedef __u32 __le32;

typedef __u32 __be32;

typedef __u64 __le64;

typedef __u64 __be64;

typedef __u16 __sum16;

typedef __u32 __wsum;

typedef unsigned int __poll_t;

// pthread_attr_t: 0x18 (24) bytes
struct pthread_attr_t
{
    unsigned int flags;
    void *stack_base;
    size_t stack_size;
    size_t guard_size;
    int sched_policy;
    int sched_priority;
};

// pthread_barrier_t: 0x20 (32) bytes
struct pthread_barrier_t
{
    int __private[8];
};

typedef int pthread_barrierattr_t;

// pthread_cond_t: 0x4 (4) bytes
struct pthread_cond_t
{
    int __private[1];
};

typedef long pthread_condattr_t;

typedef int pthread_key_t;

// pthread_mutex_t: 0x4 (4) bytes
struct pthread_mutex_t
{
    int __private[1];
};

typedef long pthread_mutexattr_t;

typedef int pthread_once_t;

// pthread_rwlock_t: 0x28 (40) bytes
struct pthread_rwlock_t
{
    int __private[10];
};

typedef long pthread_rwlockattr_t;

// pthread_spinlock_t: 0x8 (8) bytes
struct pthread_spinlock_t
{
    int __private[2];
};

typedef long pthread_t;

typedef __kernel_gid32_t __gid_t;

typedef __gid_t gid_t;

typedef __kernel_uid32_t __uid_t;

typedef __uid_t uid_t;

typedef __kernel_pid_t __pid_t;

typedef __pid_t pid_t;

typedef unsigned int __id_t;

typedef __id_t id_t;

typedef unsigned long blkcnt_t;

typedef unsigned long blksize_t;

typedef __kernel_caddr_t caddr_t;

typedef __kernel_clock_t clock_t;

typedef __kernel_clockid_t __clockid_t;

typedef __clockid_t clockid_t;

typedef __kernel_daddr_t daddr_t;

typedef unsigned long fsblkcnt_t;

typedef unsigned long fsfilcnt_t;

typedef __kernel_mode_t __mode_t;

typedef __mode_t mode_t;

typedef __kernel_key_t __key_t;

typedef __key_t key_t;

typedef __kernel_ino_t __ino_t;

typedef __ino_t ino_t;

typedef unsigned long long ino64_t;

typedef unsigned int __nlink_t;

typedef __nlink_t nlink_t;

typedef void *__timer_t;

typedef __timer_t timer_t;

typedef __kernel_suseconds_t __suseconds_t;

typedef __suseconds_t suseconds_t;

typedef unsigned int __useconds_t;

typedef __useconds_t useconds_t;

typedef unsigned int dev_t;

typedef __kernel_time_t __time_t;

typedef __time_t time_t;

typedef __kernel_off_t off_t;

typedef __kernel_loff_t loff_t;

typedef loff_t off64_t;

typedef int __socklen_t;

typedef __socklen_t socklen_t;

typedef __kernel_ssize_t ssize_t;

typedef unsigned int uint_t;

typedef unsigned int uint;

typedef unsigned char u_char;

typedef unsigned short u_short;

typedef unsigned int u_int;

typedef unsigned long u_long;

typedef unsigned int u_int32_t;

typedef unsigned short u_int16_t;

typedef unsigned char u_int8_t;

typedef unsigned long long u_int64_t;

typedef off_t fpos_t;

typedef off64_t fpos64_t;

#pragma neverd_header()
// Declared only: its members are unknown.
struct __sFILE;

#pragma neverd_header("stdio.h")
typedef struct __sFILE FILE;

#pragma neverd_header("stdlib.h")
// mallinfo: 0x28 (40) bytes
struct mallinfo
{
    size_t arena;
    size_t ordblks;
    size_t smblks;
    size_t hblks;
    size_t hblkhd;
    size_t usmblks;
    size_t fsmblks;
    size_t uordblks;
    size_t fordblks;
    size_t keepcost;
};

// mallinfo2: 0x28 (40) bytes
struct mallinfo2
{
    size_t arena;
    size_t ordblks;
    size_t smblks;
    size_t hblks;
    size_t hblkhd;
    size_t usmblks;
    size_t fsmblks;
    size_t uordblks;
    size_t fordblks;
    size_t keepcost;
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct __locale_t;

#pragma neverd_header("stdlib.h")
typedef struct __locale_t *locale_t;

// div_t: 0x8 (8) bytes
struct div_t
{
    int quot;
    int rem;
};

// ldiv_t: 0x8 (8) bytes
struct ldiv_t
{
    long quot;
    long rem;
};

// lldiv_t: 0x10 (16) bytes
struct lldiv_t
{
    NEVERD_ALIGN8 long long quot;
    NEVERD_ALIGN8 long long rem;
};

#pragma neverd_header("unistd.h")
// timespec: 0x8 (8) bytes
struct timespec
{
    time_t tv_sec;
    long tv_nsec;
};

// __kernel_timespec: 0x10 (16) bytes
struct __kernel_timespec
{
    NEVERD_ALIGN8 __kernel_time64_t tv_sec;
    NEVERD_ALIGN8 long long tv_nsec;
};

// __kernel_itimerspec: 0x20 (32) bytes
struct __kernel_itimerspec
{
    struct __kernel_timespec it_interval;
    struct __kernel_timespec it_value;
};

// __kernel_old_timespec: 0x8 (8) bytes
struct __kernel_old_timespec
{
    __kernel_old_time_t tv_sec;
    long tv_nsec;
};

// __kernel_sock_timeval: 0x10 (16) bytes
struct __kernel_sock_timeval
{
    NEVERD_ALIGN8 __s64 tv_sec;
    NEVERD_ALIGN8 __s64 tv_usec;
};

// timeval: 0x8 (8) bytes
struct timeval
{
    __kernel_old_time_t tv_sec;
    __kernel_suseconds_t tv_usec;
};

// itimerspec: 0x10 (16) bytes
struct itimerspec
{
    struct timespec it_interval;
    struct timespec it_value;
};

// itimerval: 0x10 (16) bytes
struct itimerval
{
    struct timeval it_interval;
    struct timeval it_value;
};

// timezone: 0x8 (8) bytes
struct timezone
{
    int tz_minuteswest;
    int tz_dsttime;
};

// sigcontext: 0x54 (84) bytes
struct sigcontext
{
    unsigned long trap_no;
    unsigned long error_code;
    unsigned long oldmask;
    unsigned long arm_r0;
    unsigned long arm_r1;
    unsigned long arm_r2;
    unsigned long arm_r3;
    unsigned long arm_r4;
    unsigned long arm_r5;
    unsigned long arm_r6;
    unsigned long arm_r7;
    unsigned long arm_r8;
    unsigned long arm_r9;
    unsigned long arm_r10;
    unsigned long arm_fp;
    unsigned long arm_ip;
    unsigned long arm_sp;
    unsigned long arm_lr;
    unsigned long arm_pc;
    unsigned long arm_cpsr;
    unsigned long fault_address;
};

typedef union sigval sigval_t;

// sigval: 0x4 (4) bytes
union sigval
{
    int sival_int;
    void *sival_ptr;
};

// __sifields: 0x14 (20) bytes
union __sifields
{
    struct
    {
        __kernel_pid_t _pid;
        __kernel_uid32_t _uid;
    } _kill;
    struct
    {
        __kernel_timer_t _tid;
        int _overrun;
        sigval_t _sigval;
        int _sys_private;
    } _timer;
    struct
    {
        __kernel_pid_t _pid;
        __kernel_uid32_t _uid;
        sigval_t _sigval;
    } _rt;
    struct
    {
        __kernel_pid_t _pid;
        __kernel_uid32_t _uid;
        int _status;
        __kernel_clock_t _utime;
        __kernel_clock_t _stime;
    } _sigchld;
    struct
    {
        void *_addr;
        union
        {
            int _trapno;
            short _addr_lsb;
            struct
            {
                char _dummy_bnd[4];
                void *_lower;
                void *_upper;
            } _addr_bnd;
            struct
            {
                char _dummy_pkey[4];
                __u32 _pkey;
            } _addr_pkey;
            struct
            {
                unsigned long _data;
                __u32 _type;
                __u32 _flags;
            } _perf;
        };
    } _sigfault;
    struct
    {
        long _band;
        int _fd;
    } _sigpoll;
    struct
    {
        void *_call_addr;
        int _syscall;
        unsigned int _arch;
    } _sigsys;
};

// siginfo: 0x80 (128) bytes
struct siginfo
{
    union
    {
        struct
        {
            int si_signo;
            int si_errno;
            int si_code;
            union __sifields _sifields;
        };
        int _si_pad[32];
    };
};

typedef unsigned long sigset_t;

typedef void __signalfn_t(int);

typedef __signalfn_t *__sighandler_t;

typedef void __restorefn_t(void);

typedef __restorefn_t *__sigrestore_t;

// __kernel_sigaction: 0x10 (16) bytes
struct __kernel_sigaction
{
    union
    {
        __sighandler_t _sa_handler;
        void (*_sa_sigaction)(int, struct siginfo *, void *);
    } _u;
    sigset_t sa_mask;
    unsigned long sa_flags;
    void (*sa_restorer)(void);
};

// sigaltstack: 0xC (12) bytes
struct sigaltstack
{
    void *ss_sp;
    int ss_flags;
    __kernel_size_t ss_size;
};

typedef struct sigaltstack stack_t;

typedef struct siginfo siginfo_t;

// sigevent: 0x40 (64) bytes
struct sigevent
{
    sigval_t sigev_value;
    int sigev_signo;
    int sigev_notify;
    union
    {
        int _pad[13];
        int _tid;
        struct
        {
            void (*_function)(sigval_t);
            void *_attribute;
        } _sigev_thread;
    } _sigev_un;
};

typedef struct sigevent sigevent_t;

typedef int sig_atomic_t;

typedef __sighandler_t sig_t;

typedef __sighandler_t sighandler_t;

// sigset64_t: 0x8 (8) bytes
struct sigset64_t
{
    unsigned long __bits[2];
};

// sigaction: 0x10 (16) bytes
struct sigaction
{
    union
    {
        sighandler_t sa_handler;
        void (*sa_sigaction)(int, struct siginfo *, void *);
    };
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

// sigaction64: 0x14 (20) bytes
struct sigaction64
{
    union
    {
        sighandler_t sa_handler;
        void (*sa_sigaction)(int, struct siginfo *, void *);
    };
    int sa_flags;
    void (*sa_restorer)(void);
    sigset64_t sa_mask;
};

// fp_reg: 0xC (12) bytes
struct NEVERD_GNU_STRUCT fp_reg
{
    unsigned int sign1 : 1;
    unsigned int unused : 15;
    unsigned int sign2 : 1;
    unsigned int exponent : 14;
    unsigned int j : 1;
    unsigned int mantissa1 : 31;
    unsigned int mantissa0 : 32;
};

// user_fpregs: 0x74 (116) bytes
struct NEVERD_GNU_STRUCT user_fpregs
{
    struct fp_reg fpregs[8];
    unsigned int fpsr : 32;
    unsigned int fpcr : 32;
    unsigned char ftype[8];
    unsigned int init_flag;
};

// user_regs: 0x48 (72) bytes
struct user_regs
{
    unsigned long uregs[18];
};

// user_vfp: 0x108 (264) bytes
struct user_vfp
{
    NEVERD_ALIGN8 unsigned long long fpregs[32];
    unsigned long fpscr;
};

// user_vfp_exc: 0xC (12) bytes
struct user_vfp_exc
{
    unsigned long fpexc;
    unsigned long fpinst;
    unsigned long fpinst2;
};

// user: 0x128 (296) bytes
struct user
{
    struct user_regs regs;
    int u_fpvalid;
    unsigned long u_tsize;
    unsigned long u_dsize;
    unsigned long u_ssize;
    unsigned long start_code;
    unsigned long start_stack;
    long signal;
    int reserved;
    struct user_regs *u_ar0;
    unsigned long magic;
    char u_comm[32];
    int u_debugreg[8];
    struct user_fpregs u_fp;
    struct user_fpregs *u_fp0;
};

typedef int greg_t;

typedef greg_t gregset_t[18];

typedef struct user_fpregs fpregset_t;

typedef struct sigcontext mcontext_t;

// ucontext: 0x2E8 (744) bytes
struct DECLSPEC_ALIGN(8) ucontext
{
    unsigned long uc_flags;
    struct ucontext *uc_link;
    stack_t uc_stack;
    mcontext_t uc_mcontext;
    union
    {
        struct
        {
            sigset_t uc_sigmask;
            unsigned int __padding_rt_sigset;
        };
        sigset64_t uc_sigmask64;
    };
    char __padding[120];
    unsigned long uc_regspace[128];
};

typedef struct ucontext ucontext_t;

typedef unsigned long fd_mask;

// fd_set: 0x80 (128) bytes
struct fd_set
{
    fd_mask fds_bits[32];
};

#pragma neverd_header("fcntl.h")
// flock: 0x10 (16) bytes
struct flock
{
    short l_type;
    short l_whence;
    off_t l_start;
    off_t l_len;
    pid_t l_pid;
};

// flock64: 0x20 (32) bytes
struct flock64
{
    short l_type;
    short l_whence;
    NEVERD_ALIGN8 off64_t l_start;
    NEVERD_ALIGN8 off64_t l_len;
    pid_t l_pid;
};

// f_owner_ex: 0x8 (8) bytes
struct f_owner_ex
{
    int type;
    __kernel_pid_t pid;
};

// open_how: 0x18 (24) bytes
struct open_how
{
    NEVERD_ALIGN8 __u64 flags;
    NEVERD_ALIGN8 __u64 mode;
    NEVERD_ALIGN8 __u64 resolve;
};

// statx_timestamp: 0x10 (16) bytes
struct statx_timestamp
{
    NEVERD_ALIGN8 __s64 tv_sec;
    __u32 tv_nsec;
    __s32 __reserved;
};

// statx: 0x100 (256) bytes
struct statx
{
    __u32 stx_mask;
    __u32 stx_blksize;
    NEVERD_ALIGN8 __u64 stx_attributes;
    __u32 stx_nlink;
    __u32 stx_uid;
    __u32 stx_gid;
    __u16 stx_mode;
    __u16 __spare0[1];
    NEVERD_ALIGN8 __u64 stx_ino;
    NEVERD_ALIGN8 __u64 stx_size;
    NEVERD_ALIGN8 __u64 stx_blocks;
    NEVERD_ALIGN8 __u64 stx_attributes_mask;
    struct statx_timestamp stx_atime;
    struct statx_timestamp stx_btime;
    struct statx_timestamp stx_ctime;
    struct statx_timestamp stx_mtime;
    __u32 stx_rdev_major;
    __u32 stx_rdev_minor;
    __u32 stx_dev_major;
    __u32 stx_dev_minor;
    NEVERD_ALIGN8 __u64 stx_mnt_id;
    __u32 stx_dio_mem_align;
    __u32 stx_dio_offset_align;
    NEVERD_ALIGN8 __u64 stx_subvol;
    __u32 stx_atomic_write_unit_min;
    __u32 stx_atomic_write_unit_max;
    __u32 stx_atomic_write_segments_max;
    __u32 __spare1[1];
    NEVERD_ALIGN8 __u64 __spare3[9];
};

// iovec: 0x8 (8) bytes
struct iovec
{
    void *iov_base;
    __kernel_size_t iov_len;
};

// dmabuf_cmsg: 0x18 (24) bytes
struct dmabuf_cmsg
{
    NEVERD_ALIGN8 __u64 frag_offset;
    __u32 frag_size;
    __u32 frag_token;
    __u32 dmabuf_id;
    __u32 flags;
};

// dmabuf_token: 0x8 (8) bytes
struct dmabuf_token
{
    __u32 token_start;
    __u32 token_count;
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct __timezone_t;

#pragma neverd_header("wchar.h")
typedef struct __timezone_t *timezone_t;

// tm: 0x2C (44) bytes
struct tm
{
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};

// mbstate_t: 0x4 (4) bytes
struct mbstate_t
{
    unsigned char __seq[4];
};

typedef unsigned int wint_t;

typedef long wctype_t;

typedef const void *wctrans_t;

#pragma neverd_header("locale.h")
// lconv: 0x38 (56) bytes
struct lconv
{
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
    char int_p_cs_precedes;
    char int_p_sep_by_space;
    char int_n_cs_precedes;
    char int_n_sep_by_space;
    char int_p_sign_posn;
    char int_n_sign_posn;
};

#pragma neverd_header("setjmp.h")
typedef long sigjmp_buf[65];

typedef long jmp_buf[64];

#pragma neverd_header("math.h")
typedef double __double_t;

typedef __double_t double_t;

typedef float __float_t;

typedef __float_t float_t;

#pragma neverd_header("fenv.h")
typedef __uint32_t fenv_t;

typedef __uint32_t fexcept_t;

#pragma neverd_header("inttypes.h")
// imaxdiv_t: 0x10 (16) bytes
struct imaxdiv_t
{
    NEVERD_ALIGN8 intmax_t quot;
    NEVERD_ALIGN8 intmax_t rem;
};

#pragma neverd_header("dirent.h")
// dirent: 0x118 (280) bytes
struct dirent
{
    NEVERD_ALIGN8 unsigned long long d_ino;
    NEVERD_ALIGN8 off64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[256];
};

// dirent64: 0x118 (280) bytes
struct dirent64
{
    NEVERD_ALIGN8 unsigned long long d_ino;
    NEVERD_ALIGN8 off64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[256];
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct DIR;

#pragma neverd_header("dlfcn.h")
// Dl_info: 0x10 (16) bytes
struct Dl_info
{
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
};

#pragma neverd_header("pthread.h")
// clone_args: 0x58 (88) bytes
struct clone_args
{
    NEVERD_ALIGN8 __u64 flags;
    NEVERD_ALIGN8 __u64 pidfd;
    NEVERD_ALIGN8 __u64 child_tid;
    NEVERD_ALIGN8 __u64 parent_tid;
    NEVERD_ALIGN8 __u64 exit_signal;
    NEVERD_ALIGN8 __u64 stack;
    NEVERD_ALIGN8 __u64 stack_size;
    NEVERD_ALIGN8 __u64 tls;
    NEVERD_ALIGN8 __u64 set_tid;
    NEVERD_ALIGN8 __u64 set_tid_size;
    NEVERD_ALIGN8 __u64 cgroup;
};

// sched_attr: 0x38 (56) bytes
struct sched_attr
{
    __u32 size;
    __u32 sched_policy;
    NEVERD_ALIGN8 __u64 sched_flags;
    __s32 sched_nice;
    __u32 sched_priority;
    NEVERD_ALIGN8 __u64 sched_runtime;
    NEVERD_ALIGN8 __u64 sched_deadline;
    NEVERD_ALIGN8 __u64 sched_period;
    __u32 sched_util_min;
    __u32 sched_util_max;
};

// sched_param: 0x4 (4) bytes
struct sched_param
{
    int sched_priority;
};

// cpu_set_t: 0x4 (4) bytes
struct cpu_set_t
{
    unsigned long __bits[1];
};

typedef void (*__pthread_cleanup_func_t)(void *);

// __pthread_cleanup_t: 0xC (12) bytes
struct __pthread_cleanup_t
{
    __pthread_cleanup_t *__cleanup_prev;
    __pthread_cleanup_func_t __cleanup_routine;
    void *__cleanup_arg;
};

#pragma neverd_header("semaphore.h")
// sem_t: 0x4 (4) bytes
struct sem_t
{
    unsigned int count;
};

#pragma neverd_header("poll.h")
// pollfd: 0x8 (8) bytes
struct pollfd
{
    int fd;
    short events;
    short revents;
};

typedef unsigned int nfds_t;

#pragma neverd_header("termios.h")
typedef unsigned char cc_t;

typedef unsigned int speed_t;

typedef unsigned int tcflag_t;

// termios: 0x24 (36) bytes
struct termios
{
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_line;
    cc_t c_cc[19];
};

// termios2: 0x2C (44) bytes
struct termios2
{
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_line;
    cc_t c_cc[19];
    speed_t c_ispeed;
    speed_t c_ospeed;
};

// ktermios: 0x2C (44) bytes
struct ktermios
{
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_line;
    cc_t c_cc[19];
    speed_t c_ispeed;
    speed_t c_ospeed;
};

// winsize: 0x8 (8) bytes
struct winsize
{
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

// termio: 0x12 (18) bytes
struct termio
{
    unsigned short c_iflag;
    unsigned short c_oflag;
    unsigned short c_cflag;
    unsigned short c_lflag;
    unsigned char c_line;
    unsigned char c_cc[8];
};

#pragma neverd_header("pwd.h")
// passwd: 0x18 (24) bytes
struct passwd
{
    char *pw_name;
    char *pw_passwd;
    uid_t pw_uid;
    gid_t pw_gid;
    char *pw_dir;
    char *pw_shell;
};

#pragma neverd_header("grp.h")
// group: 0x10 (16) bytes
struct group
{
    char *gr_name;
    char *gr_passwd;
    gid_t gr_gid;
    char **gr_mem;
};

#pragma neverd_header("sys/stat.h")
// stat: 0x68 (104) bytes
struct stat
{
    NEVERD_ALIGN8 unsigned long long st_dev;
    unsigned char __pad0[4];
    unsigned long __st_ino;
    unsigned int st_mode;
    nlink_t st_nlink;
    uid_t st_uid;
    gid_t st_gid;
    NEVERD_ALIGN8 unsigned long long st_rdev;
    unsigned char __pad3[4];
    NEVERD_ALIGN8 long long st_size;
    unsigned long st_blksize;
    NEVERD_ALIGN8 unsigned long long st_blocks;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
    NEVERD_ALIGN8 unsigned long long st_ino;
};

#pragma neverd_header("glob.h")
// glob_t: 0x2C (44) bytes
struct glob_t
{
    size_t gl_pathc;
    size_t gl_matchc;
    size_t gl_offs;
    int gl_flags;
    char **gl_pathv;
    int (*gl_errfunc)(const char *__failure_path, int __failure_errno);
    void (*gl_closedir)(void *);
    struct dirent *(*gl_readdir)(void *);
    void *(*gl_opendir)(const char *);
    int (*gl_lstat)(const char *, struct stat *);
    int (*gl_stat)(const char *, struct stat *);
};

#pragma neverd_header("regex.h")
typedef ssize_t regoff_t;

#pragma neverd_header()
// Declared only: its members are unknown.
struct re_guts;

#pragma neverd_header("regex.h")
// regex_t: 0x10 (16) bytes
struct regex_t
{
    int re_magic;
    size_t re_nsub;
    const char *re_endp;
    struct re_guts *re_g;
};

// regmatch_t: 0x8 (8) bytes
struct regmatch_t
{
    regoff_t rm_so;
    regoff_t rm_eo;
};

#pragma neverd_header("getopt.h")
// option: 0x10 (16) bytes
struct option
{
    const char *name;
    int has_arg;
    int *flag;
    int val;
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct __posix_spawnattr;

#pragma neverd_header("spawn.h")
typedef struct __posix_spawnattr *posix_spawnattr_t;

#pragma neverd_header()
// Declared only: its members are unknown.
struct __posix_spawn_file_actions;

#pragma neverd_header("spawn.h")
typedef struct __posix_spawn_file_actions *posix_spawn_file_actions_t;

#pragma neverd_header()
// Declared only: its members are unknown.
struct __iconv_t;

#pragma neverd_header("iconv.h")
typedef struct __iconv_t *iconv_t;

#pragma neverd_header("langinfo.h")
typedef void *nl_catd;

typedef int nl_item;

#pragma neverd_header("link.h")
typedef __u32 Elf32_Addr;

typedef __u16 Elf32_Half;

typedef __u32 Elf32_Off;

typedef __s32 Elf32_Sword;

typedef __u32 Elf32_Word;

typedef __u64 Elf64_Addr;

typedef __u16 Elf64_Half;

typedef __s16 Elf64_SHalf;

typedef __u64 Elf64_Off;

typedef __s32 Elf64_Sword;

typedef __u32 Elf64_Word;

typedef __u64 Elf64_Xword;

typedef __s64 Elf64_Sxword;

// Elf32_Dyn: 0x8 (8) bytes
struct Elf32_Dyn
{
    Elf32_Sword d_tag;
    union
    {
        Elf32_Sword d_val;
        Elf32_Addr d_ptr;
    } d_un;
};

// Elf64_Dyn: 0x10 (16) bytes
struct Elf64_Dyn
{
    NEVERD_ALIGN8 Elf64_Sxword d_tag;
    union
    {
        NEVERD_ALIGN8 Elf64_Xword d_val;
        NEVERD_ALIGN8 Elf64_Addr d_ptr;
    } d_un;
};

// elf32_rel: 0x8 (8) bytes
struct elf32_rel
{
    Elf32_Addr r_offset;
    Elf32_Word r_info;
};

typedef struct elf32_rel Elf32_Rel;

// elf64_rel: 0x10 (16) bytes
struct elf64_rel
{
    NEVERD_ALIGN8 Elf64_Addr r_offset;
    NEVERD_ALIGN8 Elf64_Xword r_info;
};

typedef struct elf64_rel Elf64_Rel;

// elf32_rela: 0xC (12) bytes
struct elf32_rela
{
    Elf32_Addr r_offset;
    Elf32_Word r_info;
    Elf32_Sword r_addend;
};

typedef struct elf32_rela Elf32_Rela;

// elf64_rela: 0x18 (24) bytes
struct elf64_rela
{
    NEVERD_ALIGN8 Elf64_Addr r_offset;
    NEVERD_ALIGN8 Elf64_Xword r_info;
    NEVERD_ALIGN8 Elf64_Sxword r_addend;
};

typedef struct elf64_rela Elf64_Rela;

// elf32_sym: 0x10 (16) bytes
struct elf32_sym
{
    Elf32_Word st_name;
    Elf32_Addr st_value;
    Elf32_Word st_size;
    unsigned char st_info;
    unsigned char st_other;
    Elf32_Half st_shndx;
};

typedef struct elf32_sym Elf32_Sym;

// elf64_sym: 0x18 (24) bytes
struct elf64_sym
{
    Elf64_Word st_name;
    unsigned char st_info;
    unsigned char st_other;
    Elf64_Half st_shndx;
    NEVERD_ALIGN8 Elf64_Addr st_value;
    NEVERD_ALIGN8 Elf64_Xword st_size;
};

typedef struct elf64_sym Elf64_Sym;

// elf32_hdr: 0x34 (52) bytes
struct elf32_hdr
{
    unsigned char e_ident[16];
    Elf32_Half e_type;
    Elf32_Half e_machine;
    Elf32_Word e_version;
    Elf32_Addr e_entry;
    Elf32_Off e_phoff;
    Elf32_Off e_shoff;
    Elf32_Word e_flags;
    Elf32_Half e_ehsize;
    Elf32_Half e_phentsize;
    Elf32_Half e_phnum;
    Elf32_Half e_shentsize;
    Elf32_Half e_shnum;
    Elf32_Half e_shstrndx;
};

typedef struct elf32_hdr Elf32_Ehdr;

// elf64_hdr: 0x40 (64) bytes
struct elf64_hdr
{
    unsigned char e_ident[16];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    NEVERD_ALIGN8 Elf64_Addr e_entry;
    NEVERD_ALIGN8 Elf64_Off e_phoff;
    NEVERD_ALIGN8 Elf64_Off e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
};

typedef struct elf64_hdr Elf64_Ehdr;

// elf32_phdr: 0x20 (32) bytes
struct elf32_phdr
{
    Elf32_Word p_type;
    Elf32_Off p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
};

typedef struct elf32_phdr Elf32_Phdr;

// elf64_phdr: 0x38 (56) bytes
struct elf64_phdr
{
    Elf64_Word p_type;
    Elf64_Word p_flags;
    NEVERD_ALIGN8 Elf64_Off p_offset;
    NEVERD_ALIGN8 Elf64_Addr p_vaddr;
    NEVERD_ALIGN8 Elf64_Addr p_paddr;
    NEVERD_ALIGN8 Elf64_Xword p_filesz;
    NEVERD_ALIGN8 Elf64_Xword p_memsz;
    NEVERD_ALIGN8 Elf64_Xword p_align;
};

typedef struct elf64_phdr Elf64_Phdr;

// elf32_shdr: 0x28 (40) bytes
struct elf32_shdr
{
    Elf32_Word sh_name;
    Elf32_Word sh_type;
    Elf32_Word sh_flags;
    Elf32_Addr sh_addr;
    Elf32_Off sh_offset;
    Elf32_Word sh_size;
    Elf32_Word sh_link;
    Elf32_Word sh_info;
    Elf32_Word sh_addralign;
    Elf32_Word sh_entsize;
};

typedef struct elf32_shdr Elf32_Shdr;

// elf64_shdr: 0x40 (64) bytes
struct elf64_shdr
{
    Elf64_Word sh_name;
    Elf64_Word sh_type;
    NEVERD_ALIGN8 Elf64_Xword sh_flags;
    NEVERD_ALIGN8 Elf64_Addr sh_addr;
    NEVERD_ALIGN8 Elf64_Off sh_offset;
    NEVERD_ALIGN8 Elf64_Xword sh_size;
    Elf64_Word sh_link;
    Elf64_Word sh_info;
    NEVERD_ALIGN8 Elf64_Xword sh_addralign;
    NEVERD_ALIGN8 Elf64_Xword sh_entsize;
};

typedef struct elf64_shdr Elf64_Shdr;

// elf32_note: 0xC (12) bytes
struct elf32_note
{
    Elf32_Word n_namesz;
    Elf32_Word n_descsz;
    Elf32_Word n_type;
};

typedef struct elf32_note Elf32_Nhdr;

// elf64_note: 0xC (12) bytes
struct elf64_note
{
    Elf64_Word n_namesz;
    Elf64_Word n_descsz;
    Elf64_Word n_type;
};

typedef struct elf64_note Elf64_Nhdr;

typedef __u64 Elf32_Xword;

typedef __s64 Elf32_Sxword;

// Elf32_auxv_t: 0x8 (8) bytes
struct Elf32_auxv_t
{
    __u32 a_type;
    union
    {
        __u32 a_val;
    } a_un;
};

// Elf64_auxv_t: 0x10 (16) bytes
struct Elf64_auxv_t
{
    NEVERD_ALIGN8 __u64 a_type;
    union
    {
        NEVERD_ALIGN8 __u64 a_val;
    } a_un;
};

// Elf32_Chdr: 0xC (12) bytes
struct Elf32_Chdr
{
    Elf32_Word ch_type;
    Elf32_Word ch_size;
    Elf32_Word ch_addralign;
};

// Elf64_Chdr: 0x18 (24) bytes
struct Elf64_Chdr
{
    Elf64_Word ch_type;
    Elf64_Word ch_reserved;
    NEVERD_ALIGN8 Elf64_Xword ch_size;
    NEVERD_ALIGN8 Elf64_Xword ch_addralign;
};

// Elf32_Lib: 0x14 (20) bytes
struct Elf32_Lib
{
    Elf32_Word l_name;
    Elf32_Word l_time_stamp;
    Elf32_Word l_checksum;
    Elf32_Word l_version;
    Elf32_Word l_flags;
};

// Elf64_Lib: 0x14 (20) bytes
struct Elf64_Lib
{
    Elf64_Word l_name;
    Elf64_Word l_time_stamp;
    Elf64_Word l_checksum;
    Elf64_Word l_version;
    Elf64_Word l_flags;
};

// Elf32_Move: 0x18 (24) bytes
struct Elf32_Move
{
    NEVERD_ALIGN8 Elf32_Xword m_value;
    Elf32_Word m_info;
    Elf32_Word m_poffset;
    Elf32_Half m_repeat;
    Elf32_Half m_stride;
};

// Elf64_Move: 0x20 (32) bytes
struct Elf64_Move
{
    NEVERD_ALIGN8 Elf64_Xword m_value;
    NEVERD_ALIGN8 Elf64_Xword m_info;
    NEVERD_ALIGN8 Elf64_Xword m_poffset;
    Elf64_Half m_repeat;
    Elf64_Half m_stride;
};

typedef __u16 Elf32_Section;

typedef __u16 Elf64_Section;

// Elf32_Syminfo: 0x4 (4) bytes
struct Elf32_Syminfo
{
    Elf32_Half si_boundto;
    Elf32_Half si_flags;
};

// Elf64_Syminfo: 0x4 (4) bytes
struct Elf64_Syminfo
{
    Elf64_Half si_boundto;
    Elf64_Half si_flags;
};

typedef Elf32_Half Elf32_Versym;

typedef Elf64_Half Elf64_Versym;

// Elf32_Verdef: 0x14 (20) bytes
struct Elf32_Verdef
{
    Elf32_Half vd_version;
    Elf32_Half vd_flags;
    Elf32_Half vd_ndx;
    Elf32_Half vd_cnt;
    Elf32_Word vd_hash;
    Elf32_Word vd_aux;
    Elf32_Word vd_next;
};

// Elf32_Verdaux: 0x8 (8) bytes
struct Elf32_Verdaux
{
    Elf32_Word vda_name;
    Elf32_Word vda_next;
};

// Elf64_Verdef: 0x14 (20) bytes
struct Elf64_Verdef
{
    Elf64_Half vd_version;
    Elf64_Half vd_flags;
    Elf64_Half vd_ndx;
    Elf64_Half vd_cnt;
    Elf64_Word vd_hash;
    Elf64_Word vd_aux;
    Elf64_Word vd_next;
};

// Elf64_Verdaux: 0x8 (8) bytes
struct Elf64_Verdaux
{
    Elf64_Word vda_name;
    Elf64_Word vda_next;
};

// Elf32_Verneed: 0x10 (16) bytes
struct Elf32_Verneed
{
    Elf32_Half vn_version;
    Elf32_Half vn_cnt;
    Elf32_Word vn_file;
    Elf32_Word vn_aux;
    Elf32_Word vn_next;
};

// Elf32_Vernaux: 0x10 (16) bytes
struct Elf32_Vernaux
{
    Elf32_Word vna_hash;
    Elf32_Half vna_flags;
    Elf32_Half vna_other;
    Elf32_Word vna_name;
    Elf32_Word vna_next;
};

// Elf64_Verneed: 0x10 (16) bytes
struct Elf64_Verneed
{
    Elf64_Half vn_version;
    Elf64_Half vn_cnt;
    Elf64_Word vn_file;
    Elf64_Word vn_aux;
    Elf64_Word vn_next;
};

// Elf64_Vernaux: 0x10 (16) bytes
struct Elf64_Vernaux
{
    Elf64_Word vna_hash;
    Elf64_Half vna_flags;
    Elf64_Half vna_other;
    Elf64_Word vna_name;
    Elf64_Word vna_next;
};

typedef Elf32_Word Elf32_Relr;

typedef Elf64_Xword Elf64_Relr;

// dl_phdr_info: 0x28 (40) bytes
struct dl_phdr_info
{
    Elf32_Addr dlpi_addr;
    const char *dlpi_name;
    const Elf32_Phdr *dlpi_phdr;
    Elf32_Half dlpi_phnum;
    NEVERD_ALIGN8 unsigned long long dlpi_adds;
    NEVERD_ALIGN8 unsigned long long dlpi_subs;
    size_t dlpi_tls_modid;
    void *dlpi_tls_data;
};

typedef uintptr_t _Unwind_Ptr;

// link_map: 0x14 (20) bytes
struct link_map
{
    Elf32_Addr l_addr;
    char *l_name;
    Elf32_Dyn *l_ld;
    struct link_map *l_next;
    struct link_map *l_prev;
};

// r_debug: 0x14 (20) bytes
struct r_debug
{
    int r_version;
    struct link_map *r_map;
    Elf32_Addr r_brk;
    enum
    {
        RT_CONSISTENT = 0,
        RT_ADD = 1,
        RT_DELETE = 2,
    } r_state;
    Elf32_Addr r_ldbase;
};

#pragma neverd_header("netdb.h")
typedef unsigned short sa_family_t;

// sockaddr_storage: 0x80 (128) bytes
struct sockaddr_storage
{
    union
    {
        struct
        {
            sa_family_t ss_family;
            char __data[126];
        };
        void *__align;
    };
};

// sockaddr: 0x10 (16) bytes
struct sockaddr
{
    sa_family_t sa_family;
    char sa_data[14];
};

// linger: 0x8 (8) bytes
struct linger
{
    int l_onoff;
    int l_linger;
};

// msghdr: 0x1C (28) bytes
struct msghdr
{
    void *msg_name;
    socklen_t msg_namelen;
    struct iovec *msg_iov;
    size_t msg_iovlen;
    void *msg_control;
    size_t msg_controllen;
    int msg_flags;
};

// mmsghdr: 0x20 (32) bytes
struct mmsghdr
{
    struct msghdr msg_hdr;
    unsigned int msg_len;
};

// cmsghdr: 0xC (12) bytes
struct cmsghdr
{
    size_t cmsg_len;
    int cmsg_level;
    int cmsg_type;
};

// ucred: 0xC (12) bytes
struct ucred
{
    pid_t pid;
    uid_t uid;
    gid_t gid;
};

// hostent: 0x14 (20) bytes
struct hostent
{
    char *h_name;
    char **h_aliases;
    int h_addrtype;
    int h_length;
    char **h_addr_list;
};

// netent: 0x10 (16) bytes
struct netent
{
    char *n_name;
    char **n_aliases;
    int n_addrtype;
    unsigned int n_net;
};

// servent: 0x10 (16) bytes
struct servent
{
    char *s_name;
    char **s_aliases;
    int s_port;
    char *s_proto;
};

// protoent: 0xC (12) bytes
struct protoent
{
    char *p_name;
    char **p_aliases;
    int p_proto;
};

// addrinfo: 0x20 (32) bytes
struct addrinfo
{
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    char *ai_canonname;
    struct sockaddr *ai_addr;
    struct addrinfo *ai_next;
};

#pragma neverd_header("ifaddrs.h")
// in6_addr: 0x10 (16) bytes
struct in6_addr
{
    union
    {
        __u8 u6_addr8[16];
        __be16 u6_addr16[8];
        __be32 u6_addr32[4];
    } in6_u;
};

// sockaddr_in6: 0x1C (28) bytes
struct sockaddr_in6
{
    unsigned short sin6_family;
    __be16 sin6_port;
    __be32 sin6_flowinfo;
    struct in6_addr sin6_addr;
    __u32 sin6_scope_id;
};

// ipv6_mreq: 0x14 (20) bytes
struct ipv6_mreq
{
    struct in6_addr ipv6mr_multiaddr;
    int ipv6mr_ifindex;
};

// in6_flowlabel_req: 0x20 (32) bytes
struct in6_flowlabel_req
{
    struct in6_addr flr_dst;
    __be32 flr_label;
    __u8 flr_action;
    __u8 flr_share;
    __u16 flr_flags;
    __u16 flr_expires;
    __u16 flr_linger;
    __u32 __flr_pad;
};

typedef unsigned int in_addr_t;

// in_addr: 0x4 (4) bytes
struct in_addr
{
    in_addr_t s_addr;
};

// ip_msfilter: 0x14 (20) bytes
struct ip_msfilter
{
    struct in_addr imsf_multiaddr;
    struct in_addr imsf_interface;
    unsigned int imsf_fmode;
    unsigned int imsf_numsrc;
    struct in_addr imsf_slist[1];
};

// ip_mreq_source: 0xC (12) bytes
struct ip_mreq_source
{
    struct in_addr imr_multiaddr;
    struct in_addr imr_interface;
    struct in_addr imr_sourceaddr;
};

typedef unsigned short __kernel_sa_family_t;

// ip_mreq: 0x8 (8) bytes
struct ip_mreq
{
    struct in_addr imr_multiaddr;
    struct in_addr imr_interface;
};

// ip_mreqn: 0xC (12) bytes
struct ip_mreqn
{
    struct in_addr imr_multiaddr;
    struct in_addr imr_address;
    int imr_ifindex;
};

// group_req: 0x84 (132) bytes
struct group_req
{
    __u32 gr_interface;
    struct sockaddr_storage gr_group;
};

// group_source_req: 0x104 (260) bytes
struct group_source_req
{
    __u32 gsr_interface;
    struct sockaddr_storage gsr_group;
    struct sockaddr_storage gsr_source;
};

// group_filter: 0x10C (268) bytes
struct group_filter
{
    union
    {
        struct
        {
            __u32 gf_interface_aux;
            struct sockaddr_storage gf_group_aux;
            __u32 gf_fmode_aux;
            __u32 gf_numsrc_aux;
            struct sockaddr_storage gf_slist[1];
        };
        struct
        {
            __u32 gf_interface;
            struct sockaddr_storage gf_group;
            __u32 gf_fmode;
            __u32 gf_numsrc;
            struct sockaddr_storage gf_slist_flex[];
        };
    };
};

// in_pktinfo: 0xC (12) bytes
struct in_pktinfo
{
    int ipi_ifindex;
    struct in_addr ipi_spec_dst;
    struct in_addr ipi_addr;
};

// sockaddr_in: 0x10 (16) bytes
struct sockaddr_in
{
    __kernel_sa_family_t sin_family;
    __be16 sin_port;
    struct in_addr sin_addr;
    unsigned char __pad[8];
};

// in6_pktinfo: 0x14 (20) bytes
struct in6_pktinfo
{
    struct in6_addr ipi6_addr;
    int ipi6_ifindex;
};

// ip6_mtuinfo: 0x20 (32) bytes
struct ip6_mtuinfo
{
    struct sockaddr_in6 ip6m_addr;
    __u32 ip6m_mtu;
};

// in6_ifreq: 0x18 (24) bytes
struct in6_ifreq
{
    struct in6_addr ifr6_addr;
    __u32 ifr6_prefixlen;
    int ifr6_ifindex;
};

// ipv6_rt_hdr: 0x4 (4) bytes
struct ipv6_rt_hdr
{
    __u8 nexthdr;
    __u8 hdrlen;
    __u8 type;
    __u8 segments_left;
};

// ipv6_opt_hdr: 0x2 (2) bytes, packed to 1
#pragma pack(push, 1)
struct ipv6_opt_hdr
{
    __u8 nexthdr;
    __u8 hdrlen;
};
#pragma pack(pop)

// rt0_hdr: 0x8 (8) bytes
struct rt0_hdr
{
    struct ipv6_rt_hdr rt_hdr;
    __u32 reserved;
    struct in6_addr addr[];
};

// rt2_hdr: 0x18 (24) bytes
struct rt2_hdr
{
    struct ipv6_rt_hdr rt_hdr;
    __u32 reserved;
    struct in6_addr addr;
};

// ipv6_destopt_hao: 0x12 (18) bytes, packed to 1
#pragma pack(push, 1)
struct ipv6_destopt_hao
{
    __u8 type;
    __u8 length;
    struct in6_addr addr;
};
#pragma pack(pop)

// ipv6hdr: 0x28 (40) bytes
struct NEVERD_GNU_STRUCT ipv6hdr
{
    __u8 priority : 4;
    __u8 version : 4;
    __u8 flow_lbl[3];
    __be16 payload_len;
    __u8 nexthdr;
    __u8 hop_limit;
    union
    {
        struct
        {
            struct in6_addr saddr;
            struct in6_addr daddr;
        };
        struct
        {
            struct in6_addr saddr;
            struct in6_addr daddr;
        } addrs;
    };
};

typedef unsigned short in_port_t;

// ifaddrs: 0x1C (28) bytes
struct ifaddrs
{
    struct ifaddrs *ifa_next;
    char *ifa_name;
    unsigned int ifa_flags;
    struct sockaddr *ifa_addr;
    struct sockaddr *ifa_netmask;
    union
    {
        struct sockaddr *ifu_broadaddr;
        struct sockaddr *ifu_dstaddr;
    } ifa_ifu;
    void *ifa_data;
};

#pragma neverd_header("search.h")
// entry: 0x8 (8) bytes
struct entry
{
    char *key;
    void *data;
};

typedef struct entry ENTRY;

#pragma neverd_header()
// Declared only: its members are unknown.
struct __hsearch;

#pragma neverd_header("search.h")
// hsearch_data: 0x4 (4) bytes
struct hsearch_data
{
    struct __hsearch *__hsearch;
};

#pragma neverd_header("utime.h")
// utimbuf: 0x8 (8) bytes
struct utimbuf
{
    __kernel_old_time_t actime;
    __kernel_old_time_t modtime;
};

#pragma neverd_header("utmp.h")
// lastlog: 0x1C (28) bytes
struct lastlog
{
    time_t ll_time;
    char ll_line[8];
    char ll_host[16];
};

// exit_status: 0x4 (4) bytes
struct exit_status
{
    short e_termination;
    short e_exit;
};

// utmp: 0x60 (96) bytes
struct utmp
{
    short ut_type;
    pid_t ut_pid;
    char ut_line[8];
    char ut_id[4];
    char ut_user[8];
    char ut_host[16];
    struct exit_status ut_exit;
    long ut_session;
    struct timeval ut_tv;
    int ut_addr_v6[4];
    char unused[20];
};

#pragma neverd_header("sys/stat.h")
// stat64: 0x68 (104) bytes
struct stat64
{
    NEVERD_ALIGN8 unsigned long long st_dev;
    unsigned char __pad0[4];
    unsigned long __st_ino;
    unsigned int st_mode;
    nlink_t st_nlink;
    uid_t st_uid;
    gid_t st_gid;
    NEVERD_ALIGN8 unsigned long long st_rdev;
    unsigned char __pad3[4];
    NEVERD_ALIGN8 long long st_size;
    unsigned long st_blksize;
    NEVERD_ALIGN8 unsigned long long st_blocks;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
    NEVERD_ALIGN8 unsigned long long st_ino;
};

#pragma neverd_header("sys/statvfs.h")
// statvfs: 0x2C (44) bytes
struct statvfs
{
    unsigned long f_bsize;
    unsigned long f_frsize;
    fsblkcnt_t f_blocks;
    fsblkcnt_t f_bfree;
    fsblkcnt_t f_bavail;
    fsfilcnt_t f_files;
    fsfilcnt_t f_ffree;
    fsfilcnt_t f_favail;
    unsigned long f_fsid;
    unsigned long f_flag;
    unsigned long f_namemax;
};

// statvfs64: 0x2C (44) bytes
struct statvfs64
{
    unsigned long f_bsize;
    unsigned long f_frsize;
    fsblkcnt_t f_blocks;
    fsblkcnt_t f_bfree;
    fsblkcnt_t f_bavail;
    fsfilcnt_t f_files;
    fsfilcnt_t f_ffree;
    fsfilcnt_t f_favail;
    unsigned long f_fsid;
    unsigned long f_flag;
    unsigned long f_namemax;
};

#pragma neverd_header("sys/times.h")
// tms: 0x10 (16) bytes
struct tms
{
    __kernel_clock_t tms_utime;
    __kernel_clock_t tms_stime;
    __kernel_clock_t tms_cutime;
    __kernel_clock_t tms_cstime;
};

#pragma neverd_header("sys/wait.h")
// rusage: 0x48 (72) bytes
struct rusage
{
    struct timeval ru_utime;
    struct timeval ru_stime;
    __kernel_long_t ru_maxrss;
    __kernel_long_t ru_ixrss;
    __kernel_long_t ru_idrss;
    __kernel_long_t ru_isrss;
    __kernel_long_t ru_minflt;
    __kernel_long_t ru_majflt;
    __kernel_long_t ru_nswap;
    __kernel_long_t ru_inblock;
    __kernel_long_t ru_oublock;
    __kernel_long_t ru_msgsnd;
    __kernel_long_t ru_msgrcv;
    __kernel_long_t ru_nsignals;
    __kernel_long_t ru_nvcsw;
    __kernel_long_t ru_nivcsw;
};

// rlimit: 0x8 (8) bytes
struct rlimit
{
    __kernel_ulong_t rlim_cur;
    __kernel_ulong_t rlim_max;
};

// rlimit64: 0x10 (16) bytes
struct rlimit64
{
    NEVERD_ALIGN8 __u64 rlim_cur;
    NEVERD_ALIGN8 __u64 rlim_max;
};

typedef unsigned long rlim_t;

typedef unsigned long long rlim64_t;

typedef int idtype_t;

#pragma neverd_header("sys/mman.h")
// cachestat_range: 0x10 (16) bytes
struct cachestat_range
{
    NEVERD_ALIGN8 __u64 off;
    NEVERD_ALIGN8 __u64 len;
};

// cachestat: 0x28 (40) bytes
struct cachestat
{
    NEVERD_ALIGN8 __u64 nr_cache;
    NEVERD_ALIGN8 __u64 nr_dirty;
    NEVERD_ALIGN8 __u64 nr_writeback;
    NEVERD_ALIGN8 __u64 nr_evicted;
    NEVERD_ALIGN8 __u64 nr_recently_evicted;
};

#pragma neverd_header("sys/un.h")
// sockaddr_un: 0x6E (110) bytes
struct sockaddr_un
{
    __kernel_sa_family_t sun_family;
    char sun_path[108];
};

#pragma neverd_header("sys/epoll.h")
// epoll_data: 0x8 (8) bytes
union epoll_data
{
    void *ptr;
    int fd;
    unsigned int u32;
    NEVERD_ALIGN8 unsigned long long u64;
};

typedef union epoll_data epoll_data_t;

// epoll_event: 0x10 (16) bytes
struct epoll_event
{
    unsigned int events;
    epoll_data_t data;
};

// epoll_params: 0x8 (8) bytes
struct epoll_params
{
    __u32 busy_poll_usecs;
    __u16 busy_poll_budget;
    __u8 prefer_busy_poll;
    __u8 __pad;
};

#pragma neverd_header("sys/eventfd.h")
typedef unsigned long long eventfd_t;

#pragma neverd_header("sys/inotify.h")
// inotify_event: 0x10 (16) bytes
struct inotify_event
{
    __s32 wd;
    __u32 mask;
    __u32 cookie;
    __u32 len;
    char name[];
};

#pragma neverd_header("sys/utsname.h")
// utsname: 0x186 (390) bytes
struct utsname
{
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

#pragma neverd_header("sys/prctl.h")
// prctl_mm_map: 0x68 (104) bytes
struct prctl_mm_map
{
    NEVERD_ALIGN8 __u64 start_code;
    NEVERD_ALIGN8 __u64 end_code;
    NEVERD_ALIGN8 __u64 start_data;
    NEVERD_ALIGN8 __u64 end_data;
    NEVERD_ALIGN8 __u64 start_brk;
    NEVERD_ALIGN8 __u64 brk;
    NEVERD_ALIGN8 __u64 start_stack;
    NEVERD_ALIGN8 __u64 arg_start;
    NEVERD_ALIGN8 __u64 arg_end;
    NEVERD_ALIGN8 __u64 env_start;
    NEVERD_ALIGN8 __u64 env_end;
    __u64 *auxv;
    __u32 auxv_size;
    __u32 exe_fd;
};

#pragma neverd_header("sys/ptrace.h")
// ptrace_peeksiginfo_args: 0x10 (16) bytes
struct ptrace_peeksiginfo_args
{
    NEVERD_ALIGN8 __u64 off;
    __u32 flags;
    __s32 nr;
};

// seccomp_metadata: 0x10 (16) bytes
struct seccomp_metadata
{
    NEVERD_ALIGN8 __u64 filter_off;
    NEVERD_ALIGN8 __u64 flags;
};

// ptrace_syscall_info: 0x58 (88) bytes
struct ptrace_syscall_info
{
    __u8 op;
    __u8 pad[3];
    __u32 arch;
    NEVERD_ALIGN8 __u64 instruction_pointer;
    NEVERD_ALIGN8 __u64 stack_pointer;
    union
    {
        struct
        {
            NEVERD_ALIGN8 __u64 nr;
            NEVERD_ALIGN8 __u64 args[6];
        } entry;
        struct
        {
            NEVERD_ALIGN8 __s64 rval;
            __u8 is_error;
        } exit;
        struct
        {
            NEVERD_ALIGN8 __u64 nr;
            NEVERD_ALIGN8 __u64 args[6];
            __u32 ret_data;
        } seccomp;
    };
};

// ptrace_rseq_configuration: 0x18 (24) bytes
struct ptrace_rseq_configuration
{
    NEVERD_ALIGN8 __u64 rseq_abi_pointer;
    __u32 rseq_abi_size;
    __u32 signature;
    __u32 flags;
    __u32 pad;
};

// ptrace_sud_config: 0x20 (32) bytes
struct ptrace_sud_config
{
    NEVERD_ALIGN8 __u64 mode;
    NEVERD_ALIGN8 __u64 selector;
    NEVERD_ALIGN8 __u64 offset;
    NEVERD_ALIGN8 __u64 len;
};

// pt_regs: 0x48 (72) bytes
struct pt_regs
{
    long uregs[18];
};

#pragma neverd_header("sys/sysinfo.h")
// sysinfo: 0x40 (64) bytes
struct sysinfo
{
    __kernel_long_t uptime;
    __kernel_ulong_t loads[3];
    __kernel_ulong_t totalram;
    __kernel_ulong_t freeram;
    __kernel_ulong_t sharedram;
    __kernel_ulong_t bufferram;
    __kernel_ulong_t totalswap;
    __kernel_ulong_t freeswap;
    __u16 procs;
    __u16 pad;
    __kernel_ulong_t totalhigh;
    __kernel_ulong_t freehigh;
    __u32 mem_unit;
    char _f[8];
};

#pragma neverd_header("sys/signalfd.h")
// signalfd_siginfo: 0x80 (128) bytes
struct signalfd_siginfo
{
    __u32 ssi_signo;
    __s32 ssi_errno;
    __s32 ssi_code;
    __u32 ssi_pid;
    __u32 ssi_uid;
    __s32 ssi_fd;
    __u32 ssi_tid;
    __u32 ssi_band;
    __u32 ssi_overrun;
    __u32 ssi_trapno;
    __s32 ssi_status;
    __s32 ssi_int;
    NEVERD_ALIGN8 __u64 ssi_ptr;
    NEVERD_ALIGN8 __u64 ssi_utime;
    NEVERD_ALIGN8 __u64 ssi_stime;
    NEVERD_ALIGN8 __u64 ssi_addr;
    __u16 ssi_addr_lsb;
    __u16 __pad2;
    __s32 ssi_syscall;
    NEVERD_ALIGN8 __u64 ssi_call_addr;
    __u32 ssi_arch;
    __u8 __pad[28];
};

#pragma neverd_header("sys/random.h")
// rand_pool_info: 0x8 (8) bytes
struct rand_pool_info
{
    int entropy_count;
    int buf_size;
    __u32 buf[];
};

// vgetrandom_opaque_params: 0x40 (64) bytes
struct vgetrandom_opaque_params
{
    __u32 size_of_opaque_state;
    __u32 mmap_prot;
    __u32 mmap_flags;
    __u32 reserved[13];
};

#pragma neverd_header("sys/sem.h")
// __kernel_legacy_ipc_perm: 0x10 (16) bytes
struct __kernel_legacy_ipc_perm
{
    __kernel_key_t key;
    __kernel_uid_t uid;
    __kernel_gid_t gid;
    __kernel_uid_t cuid;
    __kernel_gid_t cgid;
    __kernel_mode_t mode;
    unsigned short seq;
};

// ipc64_perm: 0x24 (36) bytes
struct ipc64_perm
{
    __kernel_key_t key;
    __kernel_uid32_t uid;
    __kernel_gid32_t gid;
    __kernel_uid32_t cuid;
    __kernel_gid32_t cgid;
    __kernel_mode_t mode;
    unsigned char __pad1[2];
    unsigned short seq;
    unsigned short __pad2;
    __kernel_ulong_t __unused1;
    __kernel_ulong_t __unused2;
};

#pragma neverd_header("sys/msg.h")
// msgbuf: 0x8 (8) bytes
struct msgbuf
{
    __kernel_long_t mtype;
    char mtext[1];
};

#pragma neverd_header("sys/sem.h")
// ipc_kludge: 0x8 (8) bytes
struct ipc_kludge
{
    struct msgbuf *msgp;
    long msgtyp;
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct sem;

// Declared only: its members are unknown.
struct sem_queue;

// Declared only: its members are unknown.
struct sem_undo;

#pragma neverd_header("sys/sem.h")
// __kernel_legacy_semid_ds: 0x2C (44) bytes
struct __kernel_legacy_semid_ds
{
    struct __kernel_legacy_ipc_perm sem_perm;
    __kernel_old_time_t sem_otime;
    __kernel_old_time_t sem_ctime;
    struct sem *sem_base;
    struct sem_queue *sem_pending;
    struct sem_queue **sem_pending_last;
    struct sem_undo *undo;
    unsigned short sem_nsems;
};

// semid64_ds: 0x40 (64) bytes
struct semid64_ds
{
    struct ipc64_perm sem_perm;
    unsigned long sem_otime;
    unsigned long sem_otime_high;
    unsigned long sem_ctime;
    unsigned long sem_ctime_high;
    unsigned long sem_nsems;
    unsigned long __unused3;
    unsigned long __unused4;
};

// sembuf: 0x6 (6) bytes
struct sembuf
{
    unsigned short sem_num;
    short sem_op;
    short sem_flg;
};

// seminfo: 0x28 (40) bytes
struct seminfo
{
    int semmap;
    int semmni;
    int semmns;
    int semmnu;
    int semmsl;
    int semopm;
    int semume;
    int semusz;
    int semvmx;
    int semaem;
};

// __kernel_legacy_semun: 0x4 (4) bytes
union __kernel_legacy_semun
{
    int val;
    struct __kernel_legacy_semid_ds *buf;
    unsigned short *array;
    struct seminfo *__buf;
    void *__pad;
};

// semun: 0x4 (4) bytes
union semun
{
    int val;
    struct semid64_ds *buf;
    unsigned short *array;
    struct seminfo *__buf;
    void *__pad;
};

#pragma neverd_header("sys/shm.h")
// __kernel_legacy_shmid_ds: 0x30 (48) bytes
struct __kernel_legacy_shmid_ds
{
    struct __kernel_legacy_ipc_perm shm_perm;
    int shm_segsz;
    __kernel_old_time_t shm_atime;
    __kernel_old_time_t shm_dtime;
    __kernel_old_time_t shm_ctime;
    __kernel_ipc_pid_t shm_cpid;
    __kernel_ipc_pid_t shm_lpid;
    unsigned short shm_nattch;
    unsigned short shm_unused;
    void *shm_unused2;
    void *shm_unused3;
};

// shmid64_ds: 0x54 (84) bytes
struct shmid64_ds
{
    struct ipc64_perm shm_perm;
    __kernel_size_t shm_segsz;
    unsigned long shm_atime;
    unsigned long shm_atime_high;
    unsigned long shm_dtime;
    unsigned long shm_dtime_high;
    unsigned long shm_ctime;
    unsigned long shm_ctime_high;
    __kernel_pid_t shm_cpid;
    __kernel_pid_t shm_lpid;
    unsigned long shm_nattch;
    unsigned long __unused4;
    unsigned long __unused5;
};

// shminfo64: 0x24 (36) bytes
struct shminfo64
{
    unsigned long shmmax;
    unsigned long shmmin;
    unsigned long shmmni;
    unsigned long shmseg;
    unsigned long shmall;
    unsigned long __unused1;
    unsigned long __unused2;
    unsigned long __unused3;
    unsigned long __unused4;
};

// shminfo: 0x14 (20) bytes
struct shminfo
{
    int shmmax;
    int shmmin;
    int shmmni;
    int shmseg;
    int shmall;
};

// shm_info: 0x18 (24) bytes
struct shm_info
{
    int used_ids;
    __kernel_ulong_t shm_tot;
    __kernel_ulong_t shm_rss;
    __kernel_ulong_t shm_swp;
    __kernel_ulong_t swap_attempts;
    __kernel_ulong_t swap_successes;
};

typedef unsigned long shmatt_t;

#pragma neverd_header()
// Declared only: its members are unknown.
struct msg;

#pragma neverd_header("sys/msg.h")
// __kernel_legacy_msqid_ds: 0x38 (56) bytes
struct __kernel_legacy_msqid_ds
{
    struct __kernel_legacy_ipc_perm msg_perm;
    struct msg *msg_first;
    struct msg *msg_last;
    __kernel_old_time_t msg_stime;
    __kernel_old_time_t msg_rtime;
    __kernel_old_time_t msg_ctime;
    unsigned long msg_lcbytes;
    unsigned long msg_lqbytes;
    unsigned short msg_cbytes;
    unsigned short msg_qnum;
    unsigned short msg_qbytes;
    __kernel_ipc_pid_t msg_lspid;
    __kernel_ipc_pid_t msg_lrpid;
};

// msqid64_ds: 0x58 (88) bytes
struct msqid64_ds
{
    struct ipc64_perm msg_perm;
    unsigned long msg_stime;
    unsigned long msg_stime_high;
    unsigned long msg_rtime;
    unsigned long msg_rtime_high;
    unsigned long msg_ctime;
    unsigned long msg_ctime_high;
    unsigned long msg_cbytes;
    unsigned long msg_qnum;
    unsigned long msg_qbytes;
    __kernel_pid_t msg_lspid;
    __kernel_pid_t msg_lrpid;
    unsigned long __unused4;
    unsigned long __unused5;
};

// msginfo: 0x20 (32) bytes
struct msginfo
{
    int msgpool;
    int msgmap;
    int msgmax;
    int msgmnb;
    int msgmni;
    int msgssz;
    int msgtql;
    unsigned short msgseg;
};

typedef __kernel_ulong_t msgqnum_t;

typedef __kernel_ulong_t msglen_t;

#pragma neverd_header("netinet/tcp.h")
// tcphdr: 0x14 (20) bytes
struct tcphdr
{
    union
    {
        struct NEVERD_GNU_STRUCT
        {
            unsigned short th_sport;
            unsigned short th_dport;
            unsigned int th_seq;
            unsigned int th_ack;
            unsigned char th_x2 : 4;
            unsigned char th_off : 4;
            unsigned char th_flags;
            unsigned short th_win;
            unsigned short th_sum;
            unsigned short th_urp;
        };
        struct NEVERD_GNU_STRUCT
        {
            unsigned short source;
            unsigned short dest;
            unsigned int seq;
            unsigned int ack_seq;
            unsigned short res1 : 4;
            unsigned short doff : 4;
            unsigned short fin : 1;
            unsigned short syn : 1;
            unsigned short rst : 1;
            unsigned short psh : 1;
            unsigned short ack : 1;
            unsigned short urg : 1;
            unsigned short res2 : 2;
            unsigned short window;
            unsigned short check;
            unsigned short urg_ptr;
        };
    };
};

// tcp_word_hdr: 0x14 (20) bytes
union tcp_word_hdr
{
    struct tcphdr hdr;
    __be32 words[5];
};

// tcp_repair_opt: 0x8 (8) bytes
struct tcp_repair_opt
{
    __u32 opt_code;
    __u32 opt_val;
};

// tcp_repair_window: 0x14 (20) bytes
struct tcp_repair_window
{
    __u32 snd_wl1;
    __u32 snd_wnd;
    __u32 max_window;
    __u32 rcv_wnd;
    __u32 rcv_wup;
};

// tcp_info: 0xF8 (248) bytes
struct NEVERD_GNU_STRUCT tcp_info
{
    __u8 tcpi_state;
    __u8 tcpi_ca_state;
    __u8 tcpi_retransmits;
    __u8 tcpi_probes;
    __u8 tcpi_backoff;
    __u8 tcpi_options;
    __u8 tcpi_snd_wscale : 4;
    __u8 tcpi_rcv_wscale : 4;
    __u8 tcpi_delivery_rate_app_limited : 1;
    __u8 tcpi_fastopen_client_fail : 2;
    __u32 tcpi_rto;
    __u32 tcpi_ato;
    __u32 tcpi_snd_mss;
    __u32 tcpi_rcv_mss;
    __u32 tcpi_unacked;
    __u32 tcpi_sacked;
    __u32 tcpi_lost;
    __u32 tcpi_retrans;
    __u32 tcpi_fackets;
    __u32 tcpi_last_data_sent;
    __u32 tcpi_last_ack_sent;
    __u32 tcpi_last_data_recv;
    __u32 tcpi_last_ack_recv;
    __u32 tcpi_pmtu;
    __u32 tcpi_rcv_ssthresh;
    __u32 tcpi_rtt;
    __u32 tcpi_rttvar;
    __u32 tcpi_snd_ssthresh;
    __u32 tcpi_snd_cwnd;
    __u32 tcpi_advmss;
    __u32 tcpi_reordering;
    __u32 tcpi_rcv_rtt;
    __u32 tcpi_rcv_space;
    __u32 tcpi_total_retrans;
    NEVERD_ALIGN8 __u64 tcpi_pacing_rate;
    NEVERD_ALIGN8 __u64 tcpi_max_pacing_rate;
    NEVERD_ALIGN8 __u64 tcpi_bytes_acked;
    NEVERD_ALIGN8 __u64 tcpi_bytes_received;
    __u32 tcpi_segs_out;
    __u32 tcpi_segs_in;
    __u32 tcpi_notsent_bytes;
    __u32 tcpi_min_rtt;
    __u32 tcpi_data_segs_in;
    __u32 tcpi_data_segs_out;
    NEVERD_ALIGN8 __u64 tcpi_delivery_rate;
    NEVERD_ALIGN8 __u64 tcpi_busy_time;
    NEVERD_ALIGN8 __u64 tcpi_rwnd_limited;
    NEVERD_ALIGN8 __u64 tcpi_sndbuf_limited;
    __u32 tcpi_delivered;
    __u32 tcpi_delivered_ce;
    NEVERD_ALIGN8 __u64 tcpi_bytes_sent;
    NEVERD_ALIGN8 __u64 tcpi_bytes_retrans;
    __u32 tcpi_dsack_dups;
    __u32 tcpi_reord_seen;
    __u32 tcpi_rcv_ooopack;
    __u32 tcpi_snd_wnd;
    __u32 tcpi_rcv_wnd;
    __u32 tcpi_rehash;
    __u16 tcpi_total_rto;
    __u16 tcpi_total_rto_recoveries;
    __u32 tcpi_total_rto_time;
};

// tcp_md5sig: 0xD8 (216) bytes
struct tcp_md5sig
{
    struct sockaddr_storage tcpm_addr;
    __u8 tcpm_flags;
    __u8 tcpm_prefixlen;
    __u16 tcpm_keylen;
    int tcpm_ifindex;
    __u8 tcpm_key[80];
};

// tcp_diag_md5sig: 0x64 (100) bytes
struct tcp_diag_md5sig
{
    __u8 tcpm_family;
    __u8 tcpm_prefixlen;
    __u16 tcpm_keylen;
    __be32 tcpm_addr[4];
    __u8 tcpm_key[80];
};

// tcp_ao_add: 0x120 (288) bytes
struct DECLSPEC_ALIGN(8) NEVERD_GNU_STRUCT tcp_ao_add
{
    struct sockaddr_storage addr;
    char alg_name[64];
    __s32 ifindex;
    __u32 set_current : 1;
    __u32 set_rnext : 1;
    __u32 reserved : 30;
    __u16 reserved2;
    __u8 prefix;
    __u8 sndid;
    __u8 rcvid;
    __u8 maclen;
    __u8 keyflags;
    __u8 keylen;
    __u8 key[80];
};

// tcp_ao_del: 0x90 (144) bytes
struct DECLSPEC_ALIGN(8) NEVERD_GNU_STRUCT tcp_ao_del
{
    struct sockaddr_storage addr;
    __s32 ifindex;
    __u32 set_current : 1;
    __u32 set_rnext : 1;
    __u32 del_async : 1;
    __u32 reserved : 29;
    __u16 reserved2;
    __u8 prefix;
    __u8 sndid;
    __u8 rcvid;
    __u8 current_key;
    __u8 rnext;
    __u8 keyflags;
};

// tcp_ao_info_opt: 0x30 (48) bytes
struct DECLSPEC_ALIGN(8) NEVERD_GNU_STRUCT tcp_ao_info_opt
{
    __u32 set_current : 1;
    __u32 set_rnext : 1;
    __u32 ao_required : 1;
    __u32 set_counters : 1;
    __u32 accept_icmps : 1;
    __u32 reserved : 27;
    __u16 reserved2;
    __u8 current_key;
    __u8 rnext;
    NEVERD_ALIGN8 __u64 pkt_good;
    NEVERD_ALIGN8 __u64 pkt_bad;
    NEVERD_ALIGN8 __u64 pkt_key_not_found;
    NEVERD_ALIGN8 __u64 pkt_ao_required;
    NEVERD_ALIGN8 __u64 pkt_dropped_icmp;
};

// tcp_ao_getsockopt: 0x130 (304) bytes
struct DECLSPEC_ALIGN(8) NEVERD_GNU_STRUCT tcp_ao_getsockopt
{
    struct sockaddr_storage addr;
    char alg_name[64];
    __u8 key[80];
    __u32 nkeys;
    __u16 is_current : 1;
    __u16 is_rnext : 1;
    __u16 get_all : 1;
    __u16 reserved : 13;
    __u8 sndid;
    __u8 rcvid;
    __u8 prefix;
    __u8 maclen;
    __u8 keyflags;
    __u8 keylen;
    __s32 ifindex;
    NEVERD_ALIGN8 __u64 pkt_good;
    NEVERD_ALIGN8 __u64 pkt_bad;
};

// tcp_ao_repair: 0x10 (16) bytes
struct DECLSPEC_ALIGN(8) tcp_ao_repair
{
    __be32 snt_isn;
    __be32 rcv_isn;
    __u32 snd_sne;
    __u32 rcv_sne;
};

// tcp_zerocopy_receive: 0x40 (64) bytes
struct tcp_zerocopy_receive
{
    NEVERD_ALIGN8 __u64 address;
    __u32 length;
    __u32 recv_skip_hint;
    __u32 inq;
    __s32 err;
    NEVERD_ALIGN8 __u64 copybuf_address;
    __s32 copybuf_len;
    __u32 flags;
    NEVERD_ALIGN8 __u64 msg_control;
    NEVERD_ALIGN8 __u64 msg_controllen;
    __u32 msg_flags;
    __u32 reserved;
};

#pragma neverd_header("net/if.h")
// sync_serial_settings: 0xC (12) bytes
struct sync_serial_settings
{
    unsigned int clock_rate;
    unsigned int clock_type;
    unsigned short loopback;
};

// te1_settings: 0x10 (16) bytes
struct te1_settings
{
    unsigned int clock_rate;
    unsigned int clock_type;
    unsigned short loopback;
    unsigned int slot_map;
};

// raw_hdlc_proto: 0x4 (4) bytes
struct raw_hdlc_proto
{
    unsigned short encoding;
    unsigned short parity;
};

// fr_proto: 0x18 (24) bytes
struct fr_proto
{
    unsigned int t391;
    unsigned int t392;
    unsigned int n391;
    unsigned int n392;
    unsigned int n393;
    unsigned short lmi;
    unsigned short dce;
};

// fr_proto_pvc: 0x4 (4) bytes
struct fr_proto_pvc
{
    unsigned int dlci;
};

// fr_proto_pvc_info: 0x14 (20) bytes
struct fr_proto_pvc_info
{
    unsigned int dlci;
    char master[16];
};

// cisco_proto: 0x8 (8) bytes
struct cisco_proto
{
    unsigned int interval;
    unsigned int timeout;
};

// x25_hdlc_proto: 0x18 (24) bytes
struct x25_hdlc_proto
{
    unsigned short dce;
    unsigned int modulo;
    unsigned int window;
    unsigned int t1;
    unsigned int t2;
    unsigned int n2;
};

// ifmap: 0x10 (16) bytes
struct ifmap
{
    unsigned long mem_start;
    unsigned long mem_end;
    unsigned short base_addr;
    unsigned char irq;
    unsigned char dma;
    unsigned char port;
};

// if_settings: 0xC (12) bytes
struct if_settings
{
    unsigned int type;
    unsigned int size;
    union
    {
        raw_hdlc_proto *raw_hdlc;
        cisco_proto *cisco;
        fr_proto *fr;
        fr_proto_pvc *fr_pvc;
        fr_proto_pvc_info *fr_pvc_info;
        x25_hdlc_proto *x25;
        sync_serial_settings *sync;
        te1_settings *te1;
    } ifs_ifsu;
};

// ifreq: 0x20 (32) bytes
struct ifreq
{
    union
    {
        char ifrn_name[16];
    } ifr_ifrn;
    union
    {
        struct sockaddr ifru_addr;
        struct sockaddr ifru_dstaddr;
        struct sockaddr ifru_broadaddr;
        struct sockaddr ifru_netmask;
        struct sockaddr ifru_hwaddr;
        short ifru_flags;
        int ifru_ivalue;
        int ifru_mtu;
        struct ifmap ifru_map;
        char ifru_slave[16];
        char ifru_newname[16];
        void *ifru_data;
        struct if_settings ifru_settings;
    } ifr_ifru;
};

// ifconf: 0x8 (8) bytes
struct ifconf
{
    int ifc_len;
    union
    {
        char *ifcu_buf;
        struct ifreq *ifcu_req;
    } ifc_ifcu;
};

// if_nameindex: 0x8 (8) bytes
struct if_nameindex
{
    unsigned int if_index;
    char *if_name;
};

#pragma neverd_header("fts.h")
// _ftsent: 0x48 (72) bytes
struct _ftsent
{
    struct _ftsent *fts_cycle;
    struct _ftsent *fts_parent;
    struct _ftsent *fts_link;
    long fts_number;
    void *fts_pointer;
    char *fts_accpath;
    char *fts_path;
    int fts_errno;
    int fts_symfd;
    size_t fts_pathlen;
    size_t fts_namelen;
    ino_t fts_ino;
    dev_t fts_dev;
    nlink_t fts_nlink;
    short fts_level;
    unsigned short fts_info;
    unsigned short fts_flags;
    unsigned short fts_instr;
    struct stat *fts_statp;
    char fts_name[1];
};

typedef struct _ftsent FTSENT;

// FTS: 0x28 (40) bytes
struct FTS
{
    struct _ftsent *fts_cur;
    struct _ftsent *fts_child;
    struct _ftsent **fts_array;
    dev_t fts_dev;
    char *fts_path;
    int fts_rfd;
    size_t fts_pathlen;
    int fts_nitems;
    int (*fts_compar)(const FTSENT **, const FTSENT **);
    int fts_options;
};

#pragma neverd_header("ftw.h")
// FTW: 0x8 (8) bytes
struct FTW
{
    int base;
    int level;
};

#pragma neverd_header("jni.h")
typedef unsigned char jboolean;

typedef signed char jbyte;

typedef unsigned short jchar;

typedef short jshort;

typedef int jint;

typedef long long jlong;

typedef float jfloat;

typedef double jdouble;

typedef jint jsize;

typedef void *jobject;

typedef jobject jclass;

typedef jobject jstring;

typedef jobject jarray;

typedef jarray jobjectArray;

typedef jarray jbooleanArray;

typedef jarray jbyteArray;

typedef jarray jcharArray;

typedef jarray jshortArray;

typedef jarray jintArray;

typedef jarray jlongArray;

typedef jarray jfloatArray;

typedef jarray jdoubleArray;

typedef jobject jthrowable;

typedef jobject jweak;

#pragma neverd_header()
// Declared only: its members are unknown.
struct _jfieldID;

#pragma neverd_header("jni.h")
typedef struct _jfieldID *jfieldID;

#pragma neverd_header()
// Declared only: its members are unknown.
struct _jmethodID;

#pragma neverd_header("jni.h")
typedef struct _jmethodID *jmethodID;

typedef const struct JNIInvokeInterface *JavaVM;

typedef const struct JNINativeInterface *JNIEnv;

// JNIInvokeInterface: 0x20 (32) bytes
struct JNIInvokeInterface
{
    void *reserved0;
    void *reserved1;
    void *reserved2;
    jint (*DestroyJavaVM)(JavaVM *);
    jint (*AttachCurrentThread)(JavaVM *, JNIEnv **, void *);
    jint (*DetachCurrentThread)(JavaVM *);
    jint (*GetEnv)(JavaVM *, void **, jint);
    jint (*AttachCurrentThreadAsDaemon)(JavaVM *, JNIEnv **, void *);
};

// jvalue: 0x8 (8) bytes
union jvalue
{
    jboolean z;
    jbyte b;
    jchar c;
    jshort s;
    jint i;
    NEVERD_ALIGN8 jlong j;
    jfloat f;
    NEVERD_ALIGN8 jdouble d;
    jobject l;
};

// JNINativeMethod: 0xC (12) bytes
struct JNINativeMethod
{
    const char *name;
    const char *signature;
    void *fnPtr;
};

// _JNIEnv: 0x4 (4) bytes
struct _JNIEnv
{
    const struct JNINativeInterface *functions;
};

// _JavaVM: 0x4 (4) bytes
struct _JavaVM
{
    const struct JNIInvokeInterface *functions;
};

// JNINativeInterface: 0x3A4 (932) bytes
struct JNINativeInterface
{
    void *reserved0;
    void *reserved1;
    void *reserved2;
    void *reserved3;
    jint (*GetVersion)(JNIEnv *);
    jclass (*DefineClass)(JNIEnv *, const char *, jobject, const jbyte *, jsize);
    jclass (*FindClass)(JNIEnv *, const char *);
    jmethodID (*FromReflectedMethod)(JNIEnv *, jobject);
    jfieldID (*FromReflectedField)(JNIEnv *, jobject);
    jobject (*ToReflectedMethod)(JNIEnv *, jclass, jmethodID, jboolean);
    jclass (*GetSuperclass)(JNIEnv *, jclass);
    jboolean (*IsAssignableFrom)(JNIEnv *, jclass, jclass);
    jobject (*ToReflectedField)(JNIEnv *, jclass, jfieldID, jboolean);
    jint (*Throw)(JNIEnv *, jthrowable);
    jint (*ThrowNew)(JNIEnv *, jclass, const char *);
    jthrowable (*ExceptionOccurred)(JNIEnv *);
    void (*ExceptionDescribe)(JNIEnv *);
    void (*ExceptionClear)(JNIEnv *);
    void (*FatalError)(JNIEnv *, const char *);
    jint (*PushLocalFrame)(JNIEnv *, jint);
    jobject (*PopLocalFrame)(JNIEnv *, jobject);
    jobject (*NewGlobalRef)(JNIEnv *, jobject);
    void (*DeleteGlobalRef)(JNIEnv *, jobject);
    void (*DeleteLocalRef)(JNIEnv *, jobject);
    jboolean (*IsSameObject)(JNIEnv *, jobject, jobject);
    jobject (*NewLocalRef)(JNIEnv *, jobject);
    jint (*EnsureLocalCapacity)(JNIEnv *, jint);
    jobject (*AllocObject)(JNIEnv *, jclass);
    jobject (*NewObject)(JNIEnv *, jclass, jmethodID, ...);
    jobject (*NewObjectV)(JNIEnv *, jclass, jmethodID, char *);
    jobject (*NewObjectA)(JNIEnv *, jclass, jmethodID, const jvalue *);
    jclass (*GetObjectClass)(JNIEnv *, jobject);
    jboolean (*IsInstanceOf)(JNIEnv *, jobject, jclass);
    jmethodID (*GetMethodID)(JNIEnv *, jclass, const char *, const char *);
    jobject (*CallObjectMethod)(JNIEnv *, jobject, jmethodID, ...);
    jobject (*CallObjectMethodV)(JNIEnv *, jobject, jmethodID, char *);
    jobject (*CallObjectMethodA)(JNIEnv *, jobject, jmethodID, const jvalue *);
    jboolean (*CallBooleanMethod)(JNIEnv *, jobject, jmethodID, ...);
    jboolean (*CallBooleanMethodV)(JNIEnv *, jobject, jmethodID, char *);
    jboolean (*CallBooleanMethodA)(JNIEnv *, jobject, jmethodID, const jvalue *);
    jbyte (*CallByteMethod)(JNIEnv *, jobject, jmethodID, ...);
    jbyte (*CallByteMethodV)(JNIEnv *, jobject, jmethodID, char *);
    jbyte (*CallByteMethodA)(JNIEnv *, jobject, jmethodID, const jvalue *);
    jchar (*CallCharMethod)(JNIEnv *, jobject, jmethodID, ...);
    jchar (*CallCharMethodV)(JNIEnv *, jobject, jmethodID, char *);
    jchar (*CallCharMethodA)(JNIEnv *, jobject, jmethodID, const jvalue *);
    jshort (*CallShortMethod)(JNIEnv *, jobject, jmethodID, ...);
    jshort (*CallShortMethodV)(JNIEnv *, jobject, jmethodID, char *);
    jshort (*CallShortMethodA)(JNIEnv *, jobject, jmethodID, const jvalue *);
    jint (*CallIntMethod)(JNIEnv *, jobject, jmethodID, ...);
    jint (*CallIntMethodV)(JNIEnv *, jobject, jmethodID, char *);
    jint (*CallIntMethodA)(JNIEnv *, jobject, jmethodID, const jvalue *);
    jlong (*CallLongMethod)(JNIEnv *, jobject, jmethodID, ...);
    jlong (*CallLongMethodV)(JNIEnv *, jobject, jmethodID, char *);
    jlong (*CallLongMethodA)(JNIEnv *, jobject, jmethodID, const jvalue *);
    jfloat (*CallFloatMethod)(JNIEnv *, jobject, jmethodID, ...);
    jfloat (*CallFloatMethodV)(JNIEnv *, jobject, jmethodID, char *);
    jfloat (*CallFloatMethodA)(JNIEnv *, jobject, jmethodID, const jvalue *);
    jdouble (*CallDoubleMethod)(JNIEnv *, jobject, jmethodID, ...);
    jdouble (*CallDoubleMethodV)(JNIEnv *, jobject, jmethodID, char *);
    jdouble (*CallDoubleMethodA)(JNIEnv *, jobject, jmethodID, const jvalue *);
    void (*CallVoidMethod)(JNIEnv *, jobject, jmethodID, ...);
    void (*CallVoidMethodV)(JNIEnv *, jobject, jmethodID, char *);
    void (*CallVoidMethodA)(JNIEnv *, jobject, jmethodID, const jvalue *);
    jobject (*CallNonvirtualObjectMethod)(JNIEnv *, jobject, jclass, jmethodID, ...);
    jobject (*CallNonvirtualObjectMethodV)(JNIEnv *, jobject, jclass, jmethodID, char *);
    jobject (*CallNonvirtualObjectMethodA)(JNIEnv *, jobject, jclass, jmethodID, const jvalue *);
    jboolean (*CallNonvirtualBooleanMethod)(JNIEnv *, jobject, jclass, jmethodID, ...);
    jboolean (*CallNonvirtualBooleanMethodV)(JNIEnv *, jobject, jclass, jmethodID, char *);
    jboolean (*CallNonvirtualBooleanMethodA)(JNIEnv *, jobject, jclass, jmethodID, const jvalue *);
    jbyte (*CallNonvirtualByteMethod)(JNIEnv *, jobject, jclass, jmethodID, ...);
    jbyte (*CallNonvirtualByteMethodV)(JNIEnv *, jobject, jclass, jmethodID, char *);
    jbyte (*CallNonvirtualByteMethodA)(JNIEnv *, jobject, jclass, jmethodID, const jvalue *);
    jchar (*CallNonvirtualCharMethod)(JNIEnv *, jobject, jclass, jmethodID, ...);
    jchar (*CallNonvirtualCharMethodV)(JNIEnv *, jobject, jclass, jmethodID, char *);
    jchar (*CallNonvirtualCharMethodA)(JNIEnv *, jobject, jclass, jmethodID, const jvalue *);
    jshort (*CallNonvirtualShortMethod)(JNIEnv *, jobject, jclass, jmethodID, ...);
    jshort (*CallNonvirtualShortMethodV)(JNIEnv *, jobject, jclass, jmethodID, char *);
    jshort (*CallNonvirtualShortMethodA)(JNIEnv *, jobject, jclass, jmethodID, const jvalue *);
    jint (*CallNonvirtualIntMethod)(JNIEnv *, jobject, jclass, jmethodID, ...);
    jint (*CallNonvirtualIntMethodV)(JNIEnv *, jobject, jclass, jmethodID, char *);
    jint (*CallNonvirtualIntMethodA)(JNIEnv *, jobject, jclass, jmethodID, const jvalue *);
    jlong (*CallNonvirtualLongMethod)(JNIEnv *, jobject, jclass, jmethodID, ...);
    jlong (*CallNonvirtualLongMethodV)(JNIEnv *, jobject, jclass, jmethodID, char *);
    jlong (*CallNonvirtualLongMethodA)(JNIEnv *, jobject, jclass, jmethodID, const jvalue *);
    jfloat (*CallNonvirtualFloatMethod)(JNIEnv *, jobject, jclass, jmethodID, ...);
    jfloat (*CallNonvirtualFloatMethodV)(JNIEnv *, jobject, jclass, jmethodID, char *);
    jfloat (*CallNonvirtualFloatMethodA)(JNIEnv *, jobject, jclass, jmethodID, const jvalue *);
    jdouble (*CallNonvirtualDoubleMethod)(JNIEnv *, jobject, jclass, jmethodID, ...);
    jdouble (*CallNonvirtualDoubleMethodV)(JNIEnv *, jobject, jclass, jmethodID, char *);
    jdouble (*CallNonvirtualDoubleMethodA)(JNIEnv *, jobject, jclass, jmethodID, const jvalue *);
    void (*CallNonvirtualVoidMethod)(JNIEnv *, jobject, jclass, jmethodID, ...);
    void (*CallNonvirtualVoidMethodV)(JNIEnv *, jobject, jclass, jmethodID, char *);
    void (*CallNonvirtualVoidMethodA)(JNIEnv *, jobject, jclass, jmethodID, const jvalue *);
    jfieldID (*GetFieldID)(JNIEnv *, jclass, const char *, const char *);
    jobject (*GetObjectField)(JNIEnv *, jobject, jfieldID);
    jboolean (*GetBooleanField)(JNIEnv *, jobject, jfieldID);
    jbyte (*GetByteField)(JNIEnv *, jobject, jfieldID);
    jchar (*GetCharField)(JNIEnv *, jobject, jfieldID);
    jshort (*GetShortField)(JNIEnv *, jobject, jfieldID);
    jint (*GetIntField)(JNIEnv *, jobject, jfieldID);
    jlong (*GetLongField)(JNIEnv *, jobject, jfieldID);
    jfloat (*GetFloatField)(JNIEnv *, jobject, jfieldID);
    jdouble (*GetDoubleField)(JNIEnv *, jobject, jfieldID);
    void (*SetObjectField)(JNIEnv *, jobject, jfieldID, jobject);
    void (*SetBooleanField)(JNIEnv *, jobject, jfieldID, jboolean);
    void (*SetByteField)(JNIEnv *, jobject, jfieldID, jbyte);
    void (*SetCharField)(JNIEnv *, jobject, jfieldID, jchar);
    void (*SetShortField)(JNIEnv *, jobject, jfieldID, jshort);
    void (*SetIntField)(JNIEnv *, jobject, jfieldID, jint);
    void (*SetLongField)(JNIEnv *, jobject, jfieldID, jlong);
    void (*SetFloatField)(JNIEnv *, jobject, jfieldID, jfloat);
    void (*SetDoubleField)(JNIEnv *, jobject, jfieldID, jdouble);
    jmethodID (*GetStaticMethodID)(JNIEnv *, jclass, const char *, const char *);
    jobject (*CallStaticObjectMethod)(JNIEnv *, jclass, jmethodID, ...);
    jobject (*CallStaticObjectMethodV)(JNIEnv *, jclass, jmethodID, char *);
    jobject (*CallStaticObjectMethodA)(JNIEnv *, jclass, jmethodID, const jvalue *);
    jboolean (*CallStaticBooleanMethod)(JNIEnv *, jclass, jmethodID, ...);
    jboolean (*CallStaticBooleanMethodV)(JNIEnv *, jclass, jmethodID, char *);
    jboolean (*CallStaticBooleanMethodA)(JNIEnv *, jclass, jmethodID, const jvalue *);
    jbyte (*CallStaticByteMethod)(JNIEnv *, jclass, jmethodID, ...);
    jbyte (*CallStaticByteMethodV)(JNIEnv *, jclass, jmethodID, char *);
    jbyte (*CallStaticByteMethodA)(JNIEnv *, jclass, jmethodID, const jvalue *);
    jchar (*CallStaticCharMethod)(JNIEnv *, jclass, jmethodID, ...);
    jchar (*CallStaticCharMethodV)(JNIEnv *, jclass, jmethodID, char *);
    jchar (*CallStaticCharMethodA)(JNIEnv *, jclass, jmethodID, const jvalue *);
    jshort (*CallStaticShortMethod)(JNIEnv *, jclass, jmethodID, ...);
    jshort (*CallStaticShortMethodV)(JNIEnv *, jclass, jmethodID, char *);
    jshort (*CallStaticShortMethodA)(JNIEnv *, jclass, jmethodID, const jvalue *);
    jint (*CallStaticIntMethod)(JNIEnv *, jclass, jmethodID, ...);
    jint (*CallStaticIntMethodV)(JNIEnv *, jclass, jmethodID, char *);
    jint (*CallStaticIntMethodA)(JNIEnv *, jclass, jmethodID, const jvalue *);
    jlong (*CallStaticLongMethod)(JNIEnv *, jclass, jmethodID, ...);
    jlong (*CallStaticLongMethodV)(JNIEnv *, jclass, jmethodID, char *);
    jlong (*CallStaticLongMethodA)(JNIEnv *, jclass, jmethodID, const jvalue *);
    jfloat (*CallStaticFloatMethod)(JNIEnv *, jclass, jmethodID, ...);
    jfloat (*CallStaticFloatMethodV)(JNIEnv *, jclass, jmethodID, char *);
    jfloat (*CallStaticFloatMethodA)(JNIEnv *, jclass, jmethodID, const jvalue *);
    jdouble (*CallStaticDoubleMethod)(JNIEnv *, jclass, jmethodID, ...);
    jdouble (*CallStaticDoubleMethodV)(JNIEnv *, jclass, jmethodID, char *);
    jdouble (*CallStaticDoubleMethodA)(JNIEnv *, jclass, jmethodID, const jvalue *);
    void (*CallStaticVoidMethod)(JNIEnv *, jclass, jmethodID, ...);
    void (*CallStaticVoidMethodV)(JNIEnv *, jclass, jmethodID, char *);
    void (*CallStaticVoidMethodA)(JNIEnv *, jclass, jmethodID, const jvalue *);
    jfieldID (*GetStaticFieldID)(JNIEnv *, jclass, const char *, const char *);
    jobject (*GetStaticObjectField)(JNIEnv *, jclass, jfieldID);
    jboolean (*GetStaticBooleanField)(JNIEnv *, jclass, jfieldID);
    jbyte (*GetStaticByteField)(JNIEnv *, jclass, jfieldID);
    jchar (*GetStaticCharField)(JNIEnv *, jclass, jfieldID);
    jshort (*GetStaticShortField)(JNIEnv *, jclass, jfieldID);
    jint (*GetStaticIntField)(JNIEnv *, jclass, jfieldID);
    jlong (*GetStaticLongField)(JNIEnv *, jclass, jfieldID);
    jfloat (*GetStaticFloatField)(JNIEnv *, jclass, jfieldID);
    jdouble (*GetStaticDoubleField)(JNIEnv *, jclass, jfieldID);
    void (*SetStaticObjectField)(JNIEnv *, jclass, jfieldID, jobject);
    void (*SetStaticBooleanField)(JNIEnv *, jclass, jfieldID, jboolean);
    void (*SetStaticByteField)(JNIEnv *, jclass, jfieldID, jbyte);
    void (*SetStaticCharField)(JNIEnv *, jclass, jfieldID, jchar);
    void (*SetStaticShortField)(JNIEnv *, jclass, jfieldID, jshort);
    void (*SetStaticIntField)(JNIEnv *, jclass, jfieldID, jint);
    void (*SetStaticLongField)(JNIEnv *, jclass, jfieldID, jlong);
    void (*SetStaticFloatField)(JNIEnv *, jclass, jfieldID, jfloat);
    void (*SetStaticDoubleField)(JNIEnv *, jclass, jfieldID, jdouble);
    jstring (*NewString)(JNIEnv *, const jchar *, jsize);
    jsize (*GetStringLength)(JNIEnv *, jstring);
    const jchar *(*GetStringChars)(JNIEnv *, jstring, jboolean *);
    void (*ReleaseStringChars)(JNIEnv *, jstring, const jchar *);
    jstring (*NewStringUTF)(JNIEnv *, const char *);
    jsize (*GetStringUTFLength)(JNIEnv *, jstring);
    const char *(*GetStringUTFChars)(JNIEnv *, jstring, jboolean *);
    void (*ReleaseStringUTFChars)(JNIEnv *, jstring, const char *);
    jsize (*GetArrayLength)(JNIEnv *, jarray);
    jobjectArray (*NewObjectArray)(JNIEnv *, jsize, jclass, jobject);
    jobject (*GetObjectArrayElement)(JNIEnv *, jobjectArray, jsize);
    void (*SetObjectArrayElement)(JNIEnv *, jobjectArray, jsize, jobject);
    jbooleanArray (*NewBooleanArray)(JNIEnv *, jsize);
    jbyteArray (*NewByteArray)(JNIEnv *, jsize);
    jcharArray (*NewCharArray)(JNIEnv *, jsize);
    jshortArray (*NewShortArray)(JNIEnv *, jsize);
    jintArray (*NewIntArray)(JNIEnv *, jsize);
    jlongArray (*NewLongArray)(JNIEnv *, jsize);
    jfloatArray (*NewFloatArray)(JNIEnv *, jsize);
    jdoubleArray (*NewDoubleArray)(JNIEnv *, jsize);
    jboolean *(*GetBooleanArrayElements)(JNIEnv *, jbooleanArray, jboolean *);
    jbyte *(*GetByteArrayElements)(JNIEnv *, jbyteArray, jboolean *);
    jchar *(*GetCharArrayElements)(JNIEnv *, jcharArray, jboolean *);
    jshort *(*GetShortArrayElements)(JNIEnv *, jshortArray, jboolean *);
    jint *(*GetIntArrayElements)(JNIEnv *, jintArray, jboolean *);
    jlong *(*GetLongArrayElements)(JNIEnv *, jlongArray, jboolean *);
    jfloat *(*GetFloatArrayElements)(JNIEnv *, jfloatArray, jboolean *);
    jdouble *(*GetDoubleArrayElements)(JNIEnv *, jdoubleArray, jboolean *);
    void (*ReleaseBooleanArrayElements)(JNIEnv *, jbooleanArray, jboolean *, jint);
    void (*ReleaseByteArrayElements)(JNIEnv *, jbyteArray, jbyte *, jint);
    void (*ReleaseCharArrayElements)(JNIEnv *, jcharArray, jchar *, jint);
    void (*ReleaseShortArrayElements)(JNIEnv *, jshortArray, jshort *, jint);
    void (*ReleaseIntArrayElements)(JNIEnv *, jintArray, jint *, jint);
    void (*ReleaseLongArrayElements)(JNIEnv *, jlongArray, jlong *, jint);
    void (*ReleaseFloatArrayElements)(JNIEnv *, jfloatArray, jfloat *, jint);
    void (*ReleaseDoubleArrayElements)(JNIEnv *, jdoubleArray, jdouble *, jint);
    void (*GetBooleanArrayRegion)(JNIEnv *, jbooleanArray, jsize, jsize, jboolean *);
    void (*GetByteArrayRegion)(JNIEnv *, jbyteArray, jsize, jsize, jbyte *);
    void (*GetCharArrayRegion)(JNIEnv *, jcharArray, jsize, jsize, jchar *);
    void (*GetShortArrayRegion)(JNIEnv *, jshortArray, jsize, jsize, jshort *);
    void (*GetIntArrayRegion)(JNIEnv *, jintArray, jsize, jsize, jint *);
    void (*GetLongArrayRegion)(JNIEnv *, jlongArray, jsize, jsize, jlong *);
    void (*GetFloatArrayRegion)(JNIEnv *, jfloatArray, jsize, jsize, jfloat *);
    void (*GetDoubleArrayRegion)(JNIEnv *, jdoubleArray, jsize, jsize, jdouble *);
    void (*SetBooleanArrayRegion)(JNIEnv *, jbooleanArray, jsize, jsize, const jboolean *);
    void (*SetByteArrayRegion)(JNIEnv *, jbyteArray, jsize, jsize, const jbyte *);
    void (*SetCharArrayRegion)(JNIEnv *, jcharArray, jsize, jsize, const jchar *);
    void (*SetShortArrayRegion)(JNIEnv *, jshortArray, jsize, jsize, const jshort *);
    void (*SetIntArrayRegion)(JNIEnv *, jintArray, jsize, jsize, const jint *);
    void (*SetLongArrayRegion)(JNIEnv *, jlongArray, jsize, jsize, const jlong *);
    void (*SetFloatArrayRegion)(JNIEnv *, jfloatArray, jsize, jsize, const jfloat *);
    void (*SetDoubleArrayRegion)(JNIEnv *, jdoubleArray, jsize, jsize, const jdouble *);
    jint (*RegisterNatives)(JNIEnv *, jclass, const JNINativeMethod *, jint);
    jint (*UnregisterNatives)(JNIEnv *, jclass);
    jint (*MonitorEnter)(JNIEnv *, jobject);
    jint (*MonitorExit)(JNIEnv *, jobject);
    jint (*GetJavaVM)(JNIEnv *, JavaVM **);
    void (*GetStringRegion)(JNIEnv *, jstring, jsize, jsize, jchar *);
    void (*GetStringUTFRegion)(JNIEnv *, jstring, jsize, jsize, char *);
    void *(*GetPrimitiveArrayCritical)(JNIEnv *, jarray, jboolean *);
    void (*ReleasePrimitiveArrayCritical)(JNIEnv *, jarray, void *, jint);
    const jchar *(*GetStringCritical)(JNIEnv *, jstring, jboolean *);
    void (*ReleaseStringCritical)(JNIEnv *, jstring, const jchar *);
    jweak (*NewWeakGlobalRef)(JNIEnv *, jobject);
    void (*DeleteWeakGlobalRef)(JNIEnv *, jweak);
    jboolean (*ExceptionCheck)(JNIEnv *);
    jobject (*NewDirectByteBuffer)(JNIEnv *, void *, jlong);
    void *(*GetDirectBufferAddress)(JNIEnv *, jobject);
    jlong (*GetDirectBufferCapacity)(JNIEnv *, jobject);
    jobjectRefType (*GetObjectRefType)(JNIEnv *, jobject);
};

typedef const struct JNINativeInterface *C_JNIEnv;

// JavaVMAttachArgs: 0xC (12) bytes
struct JavaVMAttachArgs
{
    jint version;
    const char *name;
    jobject group;
};

// JavaVMOption: 0x8 (8) bytes
struct JavaVMOption
{
    const char *optionString;
    void *extraInfo;
};

// JavaVMInitArgs: 0x10 (16) bytes
struct JavaVMInitArgs
{
    jint version;
    jint nOptions;
    JavaVMOption *options;
    jboolean ignoreUnrecognized;
};

#pragma neverd_header("android/log.h")
typedef enum log_id log_id_t;

// __android_log_message: 0x1C (28) bytes
struct __android_log_message
{
    size_t struct_size;
    int buffer_id;
    int priority;
    const char *tag;
    const char *file;
    unsigned int line;
    const char *message;
};

typedef void (*__android_logger_function)(const struct __android_log_message *log_message);

typedef void (*__android_aborter_function)(const char *abort_message);

#pragma neverd_header()
// Declared only: its members are unknown.
struct prop_info;


jint JNI_CreateJavaVM(JavaVM **, JNIEnv **, void *);
jint JNI_GetCreatedJavaVMs(JavaVM **, jsize, jsize *);
jint JNI_GetDefaultJavaVMInitArgs(void *);
jint JNI_OnLoad(JavaVM *vm, void *reserved);
void JNI_OnUnload(JavaVM *vm, void *reserved);
_Noreturn void _Exit(int __status);
pid_t _Fork(void);
void __FD_CLR_chk(int, fd_set *, size_t);
int __FD_ISSET_chk(int, const fd_set *, size_t);
void __FD_SET_chk(int, fd_set *, size_t);
_Noreturn void __android_log_assert(const char *cond, const char *tag, const char *fmt, ...);
int __android_log_buf_print(int log_id, int prio, const char *tag, const char *fmt, ...);
int __android_log_buf_write(int log_id, int prio, const char *tag, const char *text);
void __android_log_call_aborter(const char *abort_message);
_Noreturn void __android_log_default_aborter(const char *abort_message);
int __android_log_get_minimum_priority(void);
bool __android_log_id_is_valid(log_id_t log_id);
int __android_log_is_loggable(int prio, const char *tag, int default_prio);
int __android_log_is_loggable_len(int prio, const char *tag, size_t len, int default_prio);
void __android_log_logd_logger(const struct __android_log_message *log_message);
void __android_log_logd_logger_with_timestamp(const struct __android_log_message *log_message, const struct timespec *timestamp);
int __android_log_print(int prio, const char *tag, const char *fmt, ...);
void __android_log_set_aborter(__android_aborter_function aborter);
void __android_log_set_default_tag(const char *tag);
void __android_log_set_logger(__android_logger_function logger);
int __android_log_set_minimum_priority(int priority);
void __android_log_stderr_logger(const struct __android_log_message *log_message);
int __android_log_vprint(int prio, const char *tag, const char *fmt, char *ap);
int __android_log_write(int prio, const char *tag, const char *text);
void __android_log_write_log_message(struct __android_log_message *log_message);
_Noreturn void __assert(const char *__file, int __line, const char *__msg);
_Noreturn void __assert2(const char *__file, int __line, const char *__function, const char *__msg);
void __bionic_bcopy(const void *b1, void *b2, size_t len);
void __bionic_bzero(void *b, size_t len);
int __bionic_ctype_in_range(unsigned int __lo, int __ch, unsigned int __hi);
struct cmsghdr *__cmsg_nxthdr(struct msghdr *__msg, struct cmsghdr *__cmsg);
size_t __ctype_get_mb_cur_max(void);
int *__errno(void);
__u32 __fswahb32(__u32 val);
__u32 __fswahw32(__u32 val);
int *__get_h_errno(void);
char *__gnu_basename(const char *__path);
char *__gnu_strerror_r(int __errno_value, char *__buf, size_t __n);
int __libc_current_sigrtmax(void);
int __libc_current_sigrtmin(void);
char *__posix_basename(const char *__path);
void __pthread_cleanup_pop(__pthread_cleanup_t *, int);
void __pthread_cleanup_push(__pthread_cleanup_t *c, __pthread_cleanup_func_t, void *);
cpu_set_t *__sched_cpualloc(size_t __count);
int __sched_cpucount(size_t __set_size, const cpu_set_t *__set);
void __sched_cpufree(cpu_set_t *__set);
char *__strchr_chk(const char *__s, int __ch, size_t __n);
size_t __strlen_chk(const char *__s, size_t __n);
char *__strrchr_chk(const char *__s, int __ch, size_t __n);
__u16 __swab16p(const __u16 *p);
void __swab16s(__u16 *p);
__u32 __swab32p(const __u32 *p);
void __swab32s(__u32 *p);
__u64 __swab64p(const __u64 *p);
void __swab64s(__u64 *p);
__u32 __swahb32p(const __u32 *p);
void __swahb32s(__u32 *p);
__u32 __swahw32p(const __u32 *p);
void __swahw32s(__u32 *p);
int __system_properties_init(void);
int __system_properties_zygote_reload(void);
int __system_property_add(const char *__name, unsigned int __name_length, const char *__value, unsigned int __value_length);
int __system_property_area_init(void);
unsigned int __system_property_area_serial(void);
const prop_info *__system_property_find(const char *__name);
const prop_info *__system_property_find_nth(unsigned int __n);
int __system_property_foreach(void (*__callback)(const prop_info *__pi, void *__cookie), void *__cookie);
int __system_property_get(const char *__name, char *__value);
int __system_property_read(const prop_info *__pi, char *__name, char *__value);
void __system_property_read_callback(const prop_info *__pi, void (*__callback)(void *__cookie, const char *__name, const char *__value, unsigned int __serial), void *__cookie);
unsigned int __system_property_serial(const prop_info *__pi);
int __system_property_set(const char *__name, const char *__value);
int __system_property_set_filename(const char *__filename);
int __system_property_update(prop_info *__pi, const char *__value, unsigned int __value_length);
bool __system_property_wait(const prop_info *__pi, unsigned int __old_serial, unsigned int *__new_serial_ptr, const struct timespec *__relative_timeout);
unsigned int __system_property_wait_any(unsigned int __old_serial);
_Noreturn void _exit(int __status);
_Noreturn void _longjmp(jmp_buf __env, int __value);
int _setjmp(jmp_buf __env);
int _tolower(int __ch);
int _toupper(int __ch);
_Noreturn void abort(void);
int abs(int __x);
int accept(int __fd, struct sockaddr *__addr, socklen_t *__addr_length);
int accept4(int __fd, struct sockaddr *__addr, socklen_t *__addr_length, int __flags);
int access(const char *__path, int __mode);
int acct(const char *__path);
double acos(double __x);
float acosf(float __x);
double acosh(double __x);
float acoshf(float __x);
long double acoshl(long double __x);
long double acosl(long double __x);
unsigned int alarm(unsigned int __seconds);
void *aligned_alloc(size_t __alignment, size_t __size);
int alphasort(const struct dirent **__lhs, const struct dirent **__rhs);
int alphasort64(const struct dirent64 **__lhs, const struct dirent64 **__rhs);
int android_get_application_target_sdk_version(void);
int android_get_device_api_level(void);
unsigned int arc4random(void);
void arc4random_buf(void *__buf, size_t __n);
unsigned int arc4random_uniform(unsigned int __upper_bound);
char *asctime(const struct tm *__tm);
char *asctime_r(const struct tm *__tm, char *__buf);
double asin(double __x);
float asinf(float __x);
double asinh(double __x);
float asinhf(float __x);
long double asinhl(long double __x);
long double asinl(long double __x);
int asprintf(char **__s_ptr, const char *__fmt, ...);
int at_quick_exit(void (*__fn)(void));
double atan(double __x);
double atan2(double __y, double __x);
float atan2f(float __y, float __x);
long double atan2l(long double __y, long double __x);
float atanf(float __x);
double atanh(double __x);
float atanhf(float __x);
long double atanhl(long double __x);
long double atanl(long double __x);
int atexit(void (*__fn)(void));
double atof(const char *__s);
int atoi(const char *__s);
long atol(const char *__s);
long long atoll(const char *__s);
int backtrace(void **buffer, int size);
char **backtrace_symbols(void *const *buffer, int size);
void backtrace_symbols_fd(void *const *buffer, int size, int fd);
char *basename(const char *__path);
int basename_r(const char *__path, char *__buf, size_t __n);
int bind(int __fd, const struct sockaddr *__addr, socklen_t __addr_length);
int bindresvport(int __fd, struct sockaddr_in *__sin);
int brk(void *__addr);
void *bsearch(const void *__key, const void *__base, size_t __nmemb, size_t __size, int (*__comparator)(const void *__lhs, const void *__rhs));
wint_t btowc(int __ch);
int cacheflush(long __addr, long __nbytes, long __cache);
void *calloc(size_t __item_count, size_t __item_size);
int catclose(nl_catd __catalog);
char *catgets(nl_catd __catalog, int __set_number, int __msg_number, const char *__msg);
nl_catd catopen(const char *__name, int __flag);
double cbrt(double __x);
float cbrtf(float __x);
long double cbrtl(long double __x);
double ceil(double __x);
float ceilf(float __x);
long double ceill(long double __x);
speed_t cfgetispeed(const struct termios *__t);
speed_t cfgetospeed(const struct termios *__t);
void cfmakeraw(struct termios *__t);
int cfsetispeed(struct termios *_t, speed_t __speed);
int cfsetospeed(struct termios *__t, speed_t __speed);
int cfsetspeed(struct termios *__t, speed_t __speed);
int chdir(const char *__path);
int chmod(const char *__path, mode_t __mode);
int chown(const char *__path, uid_t __owner, gid_t __group);
int chroot(const char *__path);
int clearenv(void);
void clearerr(FILE *__fp);
void clearerr_unlocked(FILE *__fp);
clock_t clock(void);
int clock_getcpuclockid(pid_t __pid, clockid_t *__clock);
int clock_getres(clockid_t __clock, struct timespec *__resolution);
int clock_gettime(clockid_t __clock, struct timespec *__ts);
int clock_nanosleep(clockid_t __clock, int __flags, const struct timespec *__time, struct timespec *__remainder);
int clock_settime(clockid_t __clock, const struct timespec *__ts);
int clone(int (*__fn)(void *), void *__child_stack, int __flags, void *__arg, ...);
int close(int __fd);
int close_range(unsigned int __min_fd, unsigned int __max_fd, int __flags);
int closedir(DIR *__dir);
void closelog(void);
int connect(int __fd, const struct sockaddr *__addr, socklen_t __addr_length);
ssize_t copy_file_range(int __fd_in, off64_t *__off_in, int __fd_out, off64_t *__off_out, size_t __length, unsigned int __flags);
double copysign(double __value, double __sign);
float copysignf(float __value, float __sign);
long double copysignl(long double __value, long double __sign);
double cos(double __x);
float cosf(float __x);
double cosh(double __x);
float coshf(float __x);
long double coshl(long double __x);
long double cosl(long double __x);
int creat(const char *__path, mode_t __mode);
int creat64(const char *__path, mode_t __mode);
char *ctermid(char *__buf);
char *ctime(const time_t *__t);
char *ctime_r(const time_t *__t, char *__buf);
int daemon(int __no_chdir, int __no_close);
double difftime(time_t __lhs, time_t __rhs);
int dirfd(DIR *__dir);
char *dirname(const char *__path);
int dirname_r(const char *__path, char *__buf, size_t __n);
div_t div(int __numerator, int __denominator);
int dl_iterate_phdr(int (*__callback)(struct dl_phdr_info *__info, size_t __size, void *__data), void *__data);
_Unwind_Ptr dl_unwind_find_exidx(_Unwind_Ptr, int *);
int dladdr(const void *__addr, Dl_info *__info);
int dlclose(void *__handle);
char *dlerror(void);
void *dlopen(const char *__filename, int __flag);
void *dlsym(void *__handle, const char *__symbol);
void *dlvsym(void *__handle, const char *__symbol, const char *__version);
int dprintf(int __fd, const char *__fmt, ...);
double drand48(void);
double drem(double __x, double __y);
float dremf(float __x, float __y);
int dup(int __old_fd);
int dup2(int __old_fd, int __new_fd);
int dup3(int __old_fd, int __new_fd, int __flags);
locale_t duplocale(locale_t __l);
void endgrent(void);
void endhostent(void);
void endnetent(void);
void endprotoent(void);
void endpwent(void);
void endservent(void);
void endutent(void);
int epoll_create(int __size);
int epoll_create1(int __flags);
int epoll_ctl(int __epoll_fd, int __op, int __fd, struct epoll_event *__event);
int epoll_pwait(int __epoll_fd, struct epoll_event *__events, int __event_count, int __timeout_ms, const sigset_t *__mask);
int epoll_pwait2(int __epoll_fd, struct epoll_event *__events, int __event_count, const struct timespec *__timeout, const sigset_t *__mask);
int epoll_pwait2_64(int __epoll_fd, struct epoll_event *__events, int __event_count, const struct timespec *__timeout, const sigset64_t *__mask);
int epoll_pwait64(int __epoll_fd, struct epoll_event *__events, int __event_count, int __timeout_ms, const sigset64_t *__mask);
int epoll_wait(int __epoll_fd, struct epoll_event *__events, int __event_count, int __timeout_ms);
double erand48(unsigned short *__xsubi);
double erf(double __x);
double erfc(double __x);
float erfcf(float __x);
long double erfcl(long double __x);
float erff(float __x);
long double erfl(long double __x);
_Noreturn void err(int __status, const char *__fmt, ...);
void error(int __status, int __errno, const char *__fmt, ...);
void error_at_line(int __status, int __errno, const char *__filename, unsigned int __line_number, const char *__fmt, ...);
_Noreturn void errx(int __status, const char *__fmt, ...);
int eventfd(unsigned int __initial_value, int __flags);
int eventfd_read(int __fd, eventfd_t *__value);
int eventfd_write(int __fd, eventfd_t __value);
int execl(const char *__path, const char *__arg0, ...);
int execle(const char *__path, const char *__arg0, ...);
int execlp(const char *__file, const char *__arg0, ...);
int execv(const char *__path, char *const *__argv);
int execve(const char *__file, char *const *__argv, char *const *__envp);
int execvp(const char *__file, char *const *__argv);
int execvpe(const char *__file, char *const *__argv, char *const *__envp);
_Noreturn void exit(int __status);
double exp(double __x);
double exp2(double __x);
float exp2f(float __x);
long double exp2l(long double __x);
float expf(float __x);
long double expl(long double __x);
double expm1(double __x);
float expm1f(float __x);
long double expm1l(long double __x);
double fabs(double __x);
float fabsf(float __x);
long double fabsl(long double __x);
int faccessat(int __dirfd, const char *__path, int __mode, int __flags);
int fallocate(int __fd, int __mode, off_t __offset, off_t __length);
int fallocate64(int __fd, int __mode, off64_t __offset, off64_t __length);
int fchdir(int __fd);
int fchmod(int __fd, mode_t __mode);
int fchmodat(int __dir_fd, const char *__path, mode_t __mode, int __flags);
int fchown(int __fd, uid_t __owner, gid_t __group);
int fchownat(int __dir_fd, const char *__path, uid_t __owner, gid_t __group, int __flags);
int fclose(FILE *__fp);
int fcntl(int __fd, int __op, ...);
int fdatasync(int __fd);
double fdim(double __x, double __y);
float fdimf(float __x, float __y);
long double fdiml(long double __x, long double __y);
FILE *fdopen(int __fd, const char *__mode);
DIR *fdopendir(int __dir_fd);
int feclearexcept(int __exceptions);
int fedisableexcept(int __exceptions);
int feenableexcept(int __exceptions);
int fegetenv(fenv_t *__env);
int fegetexcept(void);
int fegetexceptflag(fexcept_t *__flag_ptr, int __exceptions);
int fegetround(void);
int feholdexcept(fenv_t *__env);
int feof(FILE *__fp);
int feof_unlocked(FILE *__fp);
int feraiseexcept(int __exceptions);
int ferror(FILE *__fp);
int ferror_unlocked(FILE *__fp);
int fesetenv(const fenv_t *__env);
int fesetexceptflag(const fexcept_t *__flag_ptr, int __exceptions);
int fesetround(int __rounding_mode);
int fetestexcept(int __exceptions);
int feupdateenv(const fenv_t *__env);
int fexecve(int __fd, char *const *__argv, char *const *__envp);
int fflush(FILE *__fp);
int fflush_unlocked(FILE *__fp);
int ffs(int __n);
int ffsl(long __n);
int ffsll(long long __n);
int fgetc(FILE *__fp);
int fgetc_unlocked(FILE *__fp);
char *fgetln(FILE *__fp, size_t *__length_ptr);
int fgetpos(FILE *__fp, fpos_t *__pos);
int fgetpos64(FILE *__fp, fpos64_t *__pos);
char *fgets(char *__buf, int __size, FILE *__fp);
char *fgets_unlocked(char *__buf, int __size, FILE *__fp);
wint_t fgetwc(FILE *__fp);
unsigned int *fgetws(unsigned int *__buf, int __size, FILE *__fp);
ssize_t fgetxattr(int __fd, const char *__name, void *__value, size_t __size);
int fileno(FILE *__fp);
int fileno_unlocked(FILE *__fp);
int finite(double __x);
int finitef(float __x);
ssize_t flistxattr(int __fd, char *__list, size_t __size);
int flock(int __fd, int __op);
void flockfile(FILE *__fp);
double floor(double __x);
float floorf(float __x);
long double floorl(long double __x);
double fma(double __x, double __y, double __z);
float fmaf(float __x, float __y, float __z);
long double fmal(long double __x, long double __y, long double __z);
double fmax(double __x, double __y);
float fmaxf(float __x, float __y);
long double fmaxl(long double __x, long double __y);
FILE *fmemopen(void *__buf, size_t __size, const char *__mode);
double fmin(double __x, double __y);
float fminf(float __x, float __y);
long double fminl(long double __x, long double __y);
double fmod(double __x, double __y);
float fmodf(float __x, float __y);
long double fmodl(long double __x, long double __y);
int fnmatch(const char *__pattern, const char *__string, int __flags);
FILE *fopen(const char *__path, const char *__mode);
FILE *fopen64(const char *__path, const char *__mode);
pid_t fork(void);
long fpathconf(int __fd, int __name);
int fprintf(FILE *__fp, const char *__fmt, ...);
int fpurge(FILE *__fp);
int fputc(int __ch, FILE *__fp);
int fputc_unlocked(int __ch, FILE *__fp);
int fputs(const char *__s, FILE *__fp);
int fputs_unlocked(const char *__s, FILE *__fp);
wint_t fputwc(unsigned int __wc, FILE *__fp);
int fputws(const unsigned int *__s, FILE *__fp);
size_t fread(void *__buf, size_t __size, size_t __count, FILE *__fp);
size_t fread_unlocked(void *__buf, size_t __size, size_t __count, FILE *__fp);
void free(void *__ptr);
void freeaddrinfo(struct addrinfo *__ptr);
void freeifaddrs(struct ifaddrs *__ptr);
void freelocale(locale_t __l);
int fremovexattr(int __fd, const char *__name);
FILE *freopen(const char *__path, const char *__mode, FILE *__fp);
FILE *freopen64(const char *__path, const char *__mode, FILE *__fp);
double frexp(double __x, int *__exponent);
float frexpf(float __x, int *__exponent);
long double frexpl(long double __x, int *__exponent);
int fscanf(FILE *__fp, const char *__fmt, ...);
int fseek(FILE *__fp, long __offset, int __whence);
int fseeko(FILE *__fp, off_t __offset, int __whence);
int fseeko64(FILE *__fp, off64_t __offset, int __whence);
int fsetpos(FILE *__fp, const fpos_t *__pos);
int fsetpos64(FILE *__fp, const fpos64_t *__pos);
int fsetxattr(int __fd, const char *__name, const void *__value, size_t __size, int __flags);
int fstat(int __fd, struct stat *__buf);
int fstat64(int __fd, struct stat64 *__buf);
int fstatat(int __dir_fd, const char *__path, struct stat *__buf, int __flags);
int fstatat64(int __dir_fd, const char *__path, struct stat64 *__buf, int __flags);
int fstatvfs(int __fd, struct statvfs *__buf);
int fstatvfs64(int __fd, struct statvfs64 *__buf);
int fsync(int __fd);
long ftell(FILE *__fp);
off_t ftello(FILE *__fp);
off64_t ftello64(FILE *__fp);
key_t ftok(const char *__path, int __id);
int ftruncate(int __fd, off_t __length);
int ftruncate64(int __fd, off64_t __length);
int ftrylockfile(FILE *__fp);
FTSENT *fts_children(FTS *__fts, int __options);
int fts_close(FTS *__fts);
FTS *fts_open(char *const *__path, int __options, int (*__comparator)(const FTSENT **__lhs, const FTSENT **__rhs));
FTSENT *fts_read(FTS *__fts);
int fts_set(FTS *__fts, FTSENT *__entry, int __options);
int ftw(const char *__dir_path, int (*__callback)(const char *, const struct stat *, int), int __max_fd_count);
int ftw64(const char *__dir_path, int (*__callback)(const char *, const struct stat64 *, int), int __max_fd_count);
void funlockfile(FILE *__fp);
FILE *funopen(const void *__cookie, int (*__read_fn)(void *, char *, int), int (*__write_fn)(void *, const char *, int), fpos_t (*__seek_fn)(void *, fpos_t, int), int (*__close_fn)(void *));
FILE *funopen64(const void *__cookie, int (*__read_fn)(void *, char *, int), int (*__write_fn)(void *, const char *, int), fpos64_t (*__seek_fn)(void *, fpos64_t, int), int (*__close_fn)(void *));
int futimens(int __fd, const struct timespec *__times);
int futimes(int __fd, const struct timeval *__times);
int futimesat(int __dir_fd, const char *__path, const struct timeval *__times);
int fwide(FILE *__fp, int __mode);
int fwprintf(FILE *__fp, const unsigned int *__fmt, ...);
size_t fwrite(const void *__buf, size_t __size, size_t __count, FILE *__fp);
size_t fwrite_unlocked(const void *__buf, size_t __size, size_t __count, FILE *__fp);
int fwscanf(FILE *__fp, const unsigned int *__fmt, ...);
const char *gai_strerror(int __error);
double gamma(double __x);
double gamma_r(double __x, int *__sign);
float gammaf(float __x);
float gammaf_r(float __x, int *__sign);
long get_avphys_pages(void);
int get_nprocs(void);
int get_nprocs_conf(void);
long get_phys_pages(void);
int getaddrinfo(const char *__node, const char *__service, const struct addrinfo *__hints, struct addrinfo **__result);
unsigned long getauxval(unsigned long __type);
int getc(FILE *__fp);
int getc_unlocked(FILE *__fp);
int getchar(void);
int getchar_unlocked(void);
char *getcwd(char *__buf, size_t __size);
ssize_t getdelim(char **__line_ptr, size_t *__line_length_ptr, int __delimiter, FILE *__fp);
int getdomainname(char *__buf, size_t __buf_size);
gid_t getegid(void);
int getentropy(void *__buffer, size_t __buffer_size);
char *getenv(const char *__name);
uid_t geteuid(void);
gid_t getgid(void);
struct group *getgrent(void);
struct group *getgrgid(gid_t __gid);
int getgrgid_r(gid_t __gid, struct group *__group, char *__buf, size_t __n, struct group **__result);
struct group *getgrnam(const char *__name);
int getgrnam_r(const char *__name, struct group *__group, char *__buf, size_t __n, struct group **__result);
int getgrouplist(const char *, gid_t __group, gid_t *__groups, int *__group_count);
int getgroups(int __size, gid_t *__list);
struct hostent *gethostbyaddr(const void *__addr, socklen_t __length, int __type);
int gethostbyaddr_r(const void *__addr, socklen_t __length, int __type, struct hostent *__ret, char *__buf, size_t __buf_size, struct hostent **__result, int *__h_errno_ptr);
struct hostent *gethostbyname(const char *__name);
struct hostent *gethostbyname2(const char *__name, int __af);
int gethostbyname2_r(const char *__name, int __af, struct hostent *__ret, char *__buf, size_t __buf_size, struct hostent **__result, int *__h_errno_ptr);
int gethostbyname_r(const char *__name, struct hostent *__ret, char *__buf, size_t __buf_size, struct hostent **__result, int *__h_errno_ptr);
struct hostent *gethostent(void);
int gethostname(char *_buf, size_t __buf_size);
int getifaddrs(struct ifaddrs **__list_ptr);
int getitimer(int __which, struct itimerval *__current_value);
ssize_t getline(char **__line_ptr, size_t *__line_length_ptr, FILE *__fp);
int getloadavg(double *__averages, int __n);
char *getlogin(void);
int getlogin_r(char *__buffer, size_t __buffer_size);
int getnameinfo(const struct sockaddr *__sa, socklen_t __sa_length, char *__host, size_t __host_length, char *__service, size_t __service_length, int __flags);
struct netent *getnetbyaddr(unsigned int __net, int __type);
struct netent *getnetbyname(const char *__name);
struct netent *getnetent(void);
int getopt(int __argc, char *const *__argv, const char *__options);
int getopt_long(int __argc, char *const *__argv, const char *__options, const struct option *__long_options, int *__long_index);
int getopt_long_only(int __argc, char *const *__argv, const char *__options, const struct option *__long_options, int *__long_index);
int getpagesize(void);
int getpeername(int __fd, struct sockaddr *__addr, socklen_t *__addr_length);
pid_t getpgid(pid_t __pid);
pid_t getpgrp(void);
pid_t getpid(void);
pid_t getppid(void);
int getpriority(int __which, id_t __who);
const char *getprogname(void);
struct protoent *getprotobyname(const char *__name);
struct protoent *getprotobynumber(int __proto);
struct protoent *getprotoent(void);
int getpt(void);
struct passwd *getpwent(void);
struct passwd *getpwnam(const char *__name);
int getpwnam_r(const char *__name, struct passwd *__pwd, char *__buf, size_t __n, struct passwd **__result);
struct passwd *getpwuid(uid_t __uid);
int getpwuid_r(uid_t __uid, struct passwd *__pwd, char *__buf, size_t __n, struct passwd **__result);
ssize_t getrandom(void *__buffer, size_t __buffer_size, unsigned int __flags);
int getresgid(gid_t *__rgid, gid_t *__egid, gid_t *__sgid);
int getresuid(uid_t *__ruid, uid_t *__euid, uid_t *__suid);
int getrlimit(int __resource, struct rlimit *__limit);
int getrlimit64(int __resource, struct rlimit64 *__limit);
int getrusage(int __who, struct rusage *__usage);
struct servent *getservbyname(const char *__name, const char *__proto);
struct servent *getservbyport(int __port_in_network_order, const char *__proto);
struct servent *getservent(void);
pid_t getsid(pid_t __pid);
int getsockname(int __fd, struct sockaddr *__addr, socklen_t *__addr_length);
int getsockopt(int __fd, int __level, int __option, void *__value, socklen_t *__value_length);
int getsubopt(char **__option, char *const *__tokens, char **__value_ptr);
pid_t gettid(void);
int gettimeofday(struct timeval *__tv, struct timezone *__tz);
uid_t getuid(void);
struct utmp *getutent(void);
wint_t getwc(FILE *__fp);
wint_t getwchar(void);
ssize_t getxattr(const char *__path, const char *__name, void *__value, size_t __size);
int glob(const char *__pattern, int __flags, int (*__error_callback)(const char *__failure_path, int __failure_errno), glob_t *__result_ptr);
void globfree(glob_t *__result_ptr);
struct tm *gmtime(const time_t *__t);
struct tm *gmtime_r(const time_t *__t, struct tm *__tm);
int grantpt(int __fd);
int hcreate(size_t __n);
int hcreate_r(size_t __n, struct hsearch_data *__table);
void hdestroy(void);
void hdestroy_r(struct hsearch_data *__table);
void herror(const char *__s);
ENTRY *hsearch(ENTRY __entry, ACTION __action);
int hsearch_r(ENTRY __entry, ACTION __action, ENTRY **__result, struct hsearch_data *__table);
const char *hstrerror(int __error);
unsigned int htonl(unsigned int __x);
unsigned short htons(unsigned short __x);
double hypot(double __x, double __y);
float hypotf(float __x, float __y);
long double hypotl(long double __x, long double __y);
size_t iconv(iconv_t __converter, char **__src_buf, size_t *__src_bytes_left, char **__dst_buf, size_t *__dst_bytes_left);
int iconv_close(iconv_t __converter);
iconv_t iconv_open(const char *__dst_encoding, const char *__src_encoding);
void if_freenameindex(struct if_nameindex *__ptr);
char *if_indextoname(unsigned int __index, char *__buf);
struct if_nameindex *if_nameindex(void);
unsigned int if_nametoindex(const char *__name);
int ilogb(double __x);
int ilogbf(float __x);
int ilogbl(long double __x);
intmax_t imaxabs(intmax_t __i);
imaxdiv_t imaxdiv(intmax_t __numerator, intmax_t __denominator);
in_addr_t inet_addr(const char *__s);
int inet_aton(const char *__s, struct in_addr *__addr);
in_addr_t inet_lnaof(struct in_addr __addr);
struct in_addr inet_makeaddr(in_addr_t __net, in_addr_t __host);
in_addr_t inet_netof(struct in_addr __addr);
in_addr_t inet_network(const char *__s);
unsigned int inet_nsap_addr(const char *__ascii, unsigned char *__binary, int __n);
char *inet_nsap_ntoa(int __binary_length, const unsigned char *__binary, char *__ascii);
char *inet_ntoa(struct in_addr __addr);
const char *inet_ntop(int __af, const void *__src, char *__dst, socklen_t __size);
int inet_pton(int __af, const char *__src, void *__dst);
int initgroups(const char *, gid_t __group);
char *initstate(unsigned int __seed, char *__state, size_t __n);
int inotify_add_watch(int __fd, const char *__path, unsigned int __mask);
int inotify_init(void);
int inotify_init1(int __flags);
int inotify_rm_watch(int __fd, unsigned int __watch_descriptor);
void insque(void *__element, void *__previous);
int ioctl(int __fd, unsigned int __op, ...);
int isalnum(int __ch);
int isalnum_l(int __ch, locale_t __l);
int isalpha(int __ch);
int isalpha_l(int __ch, locale_t __l);
int isascii(int __ch);
int isatty(int __fd);
int isblank(int __ch);
int isblank_l(int __ch, locale_t __l);
int iscntrl(int __ch);
int iscntrl_l(int __ch, locale_t __l);
int isdigit(int __ch);
int isdigit_l(int __ch, locale_t __l);
int isgraph(int __ch);
int isgraph_l(int __ch, locale_t __l);
int isinff(float __x);
int isinfl(long double __x);
int islower(int __ch);
int islower_l(int __ch, locale_t __l);
int isnanf(float __x);
int isnanl(long double __x);
int isprint(int __ch);
int isprint_l(int __ch, locale_t __l);
int ispunct(int __ch);
int ispunct_l(int __ch, locale_t __l);
int isspace(int __ch);
int isspace_l(int __ch, locale_t __l);
int isupper(int __ch);
int isupper_l(int __ch, locale_t __l);
int iswalnum(wint_t __wc);
int iswalnum_l(wint_t __wc, locale_t __l);
int iswalpha(wint_t __wc);
int iswalpha_l(wint_t __wc, locale_t __l);
int iswblank(wint_t __wc);
int iswblank_l(wint_t __wc, locale_t __l);
int iswcntrl(wint_t __wc);
int iswcntrl_l(wint_t __wc, locale_t __l);
int iswctype(wint_t __wc, wctype_t __type);
int iswctype_l(wint_t __wc, wctype_t __transform, locale_t __l);
int iswdigit(wint_t __wc);
int iswdigit_l(wint_t __wc, locale_t __l);
int iswgraph(wint_t __wc);
int iswgraph_l(wint_t __wc, locale_t __l);
int iswlower(wint_t __wc);
int iswlower_l(wint_t __wc, locale_t __l);
int iswprint(wint_t __wc);
int iswprint_l(wint_t __wc, locale_t __l);
int iswpunct(wint_t __wc);
int iswpunct_l(wint_t __wc, locale_t __l);
int iswspace(wint_t __wc);
int iswspace_l(wint_t __wc, locale_t __l);
int iswupper(wint_t __wc);
int iswupper_l(wint_t __wc, locale_t __l);
int iswxdigit(wint_t __wc);
int iswxdigit_l(wint_t __wc, locale_t __l);
int isxdigit(int __ch);
int isxdigit_l(int __ch, locale_t __l);
double j0(double __x);
float j0f(float __x);
double j1(double __x);
float j1f(float __x);
double jn(int __n, double __x);
float jnf(int __n, float __x);
long jrand48(unsigned short *__xsubi);
int kill(pid_t __pid, int __signal);
int killpg(int __pgrp, int __signal);
long labs(long __x);
int lchown(const char *__path, uid_t __owner, gid_t __group);
void lcong48(unsigned short *__param);
double ldexp(double __x, int __exponent);
float ldexpf(float __x, int __exponent);
long double ldexpl(long double __x, int __exponent);
ldiv_t ldiv(long __numerator, long __denominator);
void *lfind(const void *__key, const void *__array, size_t *__count, size_t __size, int (*__comparator)(const void *, const void *));
double lgamma(double __x);
double lgamma_r(double __x, int *__sign);
float lgammaf(float __x);
float lgammaf_r(float __x, int *__sign);
long double lgammal(long double __x);
long double lgammal_r(long double __x, int *__sign);
ssize_t lgetxattr(const char *__path, const char *__name, void *__value, size_t __size);
int link(const char *__old_path, const char *__new_path);
int linkat(int __old_dir_fd, const char *__old_path, int __new_dir_fd, const char *__new_path, int __flags);
int listen(int __fd, int __backlog);
ssize_t listxattr(const char *__path, char *__list, size_t __size);
long long llabs(long long __x);
lldiv_t lldiv(long long __numerator, long long __denominator);
ssize_t llistxattr(const char *__path, char *__list, size_t __size);
long long llrint(double __x);
long long llrintf(float __x);
long long llrintl(long double __x);
long long llround(double __x);
long long llroundf(float __x);
long long llroundl(long double __x);
struct lconv *localeconv(void);
struct tm *localtime(const time_t *__t);
struct tm *localtime_r(const time_t *__t, struct tm *__tm);
struct tm *localtime_rz(timezone_t __tz, const time_t *__t, struct tm *__tm);
int lockf(int __fd, int __op, off_t __length);
int lockf64(int __fd, int __op, off64_t __length);
double log(double __x);
double log10(double __x);
float log10f(float __x);
long double log10l(long double __x);
double log1p(double __x);
float log1pf(float __x);
long double log1pl(long double __x);
double log2(double __x);
float log2f(float __x);
long double log2l(long double __x);
double logb(double __x);
float logbf(float __x);
long double logbl(long double __x);
float logf(float __x);
int login_tty(int __fd);
long double logl(long double __x);
_Noreturn void longjmp(jmp_buf __env, int __value);
long lrand48(void);
int lremovexattr(const char *__path, const char *__name);
long lrint(double __x);
long lrintf(float __x);
long lrintl(long double __x);
long lround(double __x);
long lroundf(float __x);
long lroundl(long double __x);
void *lsearch(const void *__key, void *__array, size_t *__count, size_t __size, int (*__comparator)(const void *, const void *));
off_t lseek(int __fd, off_t __offset, int __whence);
off64_t lseek64(int __fd, off64_t __offset, int __whence);
int lsetxattr(const char *__path, const char *__name, const void *__value, size_t __size, int __flags);
int lstat(const char *__path, struct stat *__buf);
int lstat64(const char *__path, struct stat64 *__buf);
int lutimes(const char *__path, const struct timeval *__times);
int madvise(void *__addr, size_t __size, int __advice);
struct mallinfo mallinfo(void);
struct mallinfo2 mallinfo2(void);
void *malloc(size_t __byte_count);
int malloc_info(int __must_be_zero, FILE *__fp);
size_t malloc_usable_size(const void *__ptr);
int mallopt(int __option, int __value);
int mblen(const char *__s, size_t __n);
size_t mbrlen(const char *__s, size_t __n, mbstate_t *__ps);
size_t mbrtowc(unsigned int *__buf, const char *__s, size_t __n, mbstate_t *__ps);
int mbsinit(const mbstate_t *__ps);
size_t mbsnrtowcs(unsigned int *__dst, const char **__src, size_t __src_n, size_t __dst_n, mbstate_t *__ps);
size_t mbsrtowcs(unsigned int *__dst, const char **__src, size_t __dst_n, mbstate_t *__ps);
size_t mbsrtowcs_l(unsigned int *__dst, const char **__src, size_t __dst_n, mbstate_t *__ps, locale_t __l);
size_t mbstowcs(unsigned int *__dst, const char *__src, size_t __n);
int mbtowc(unsigned int *__wc_ptr, const char *__s, size_t __n);
void *memalign(size_t __alignment, size_t __byte_count);
void *memccpy(void *__dst, const void *__src, int __stop_char, size_t __n);
void *memchr(const void *__s, int __ch, size_t __n);
int memcmp(const void *__lhs, const void *__rhs, size_t __n);
void *memcpy(void *, const void *, size_t);
int memfd_create(const char *__name, unsigned int __flags);
void *memmem(const void *__haystack, size_t __haystack_size, const void *__needle, size_t __needle_size);
void *memmove(void *__dst, const void *__src, size_t __n);
void *mempcpy(void *__dst, const void *__src, size_t __n);
void *memrchr(const void *__s, int __ch, size_t __n);
void *memset(void *__dst, int __ch, size_t __n);
void *memset_explicit(void *__dst, int __ch, size_t __n);
int mincore(void *__addr, size_t __size, unsigned char *__vector);
int mkdir(const char *__path, mode_t __mode);
int mkdirat(int __dir_fd, const char *__path, mode_t __mode);
char *mkdtemp(char *__template);
int mkfifo(const char *__path, mode_t __mode);
int mkfifoat(int __dir_fd, const char *__path, mode_t __mode);
int mknod(const char *__path, mode_t __mode, dev_t __dev);
int mknodat(int __dir_fd, const char *__path, mode_t __mode, dev_t __dev);
int mkostemp(char *__template, int __flags);
int mkostemp64(char *__template, int __flags);
int mkostemps(char *__template, int __suffix_length, int __flags);
int mkostemps64(char *__template, int __suffix_length, int __flags);
int mkstemp(char *__template);
int mkstemp64(char *__template);
int mkstemps(char *__template, int __flags);
int mkstemps64(char *__template, int __flags);
char *mktemp(char *__template);
time_t mktime(struct tm *__tm);
time_t mktime_z(timezone_t __tz, struct tm *__tm);
int mlock(const void *__addr, size_t __size);
int mlock2(const void *__addr, size_t __size, int __flags);
int mlockall(int __flags);
void *mmap(void *__addr, size_t __size, int __prot, int __flags, int __fd, off_t __offset);
void *mmap64(void *__addr, size_t __size, int __prot, int __flags, int __fd, off64_t __offset);
double modf(double __x, double *__integral_part);
float modff(float __x, float *__integral_part);
long double modfl(long double __x, long double *__integral_part);
int mprotect(void *__addr, size_t __size, int __prot);
long mrand48(void);
void *mremap(void *__old_addr, size_t __old_size, size_t __new_size, int __flags, ...);
int msgctl(int __msg_id, int __op, struct msqid64_ds *__buf);
int msgget(key_t key, int __flags);
ssize_t msgrcv(int __msg_id, void *__msgbuf_ptr, size_t __size, long __type, int __flags);
int msgsnd(int __msg_id, const void *__msgbuf_ptr, size_t __size, int __flags);
int msync(void *__addr, size_t __size, int __flags);
int munlock(const void *__addr, size_t __size);
int munlockall(void);
int munmap(void *__addr, size_t __size);
double nan(const char *__kind);
float nanf(const char *__kind);
long double nanl(const char *__kind);
int nanosleep(const struct timespec *__duration, struct timespec *__remainder);
double nearbyint(double __x);
float nearbyintf(float __x);
long double nearbyintl(long double __x);
locale_t newlocale(int __category_mask, const char *__locale_name, locale_t __base);
double nextafter(double __x, double __y);
float nextafterf(float __x, float __y);
long double nextafterl(long double __x, long double __y);
double nexttoward(double __x, long double __y);
float nexttowardf(float __x, long double __y);
long double nexttowardl(long double __x, long double __y);
int nftw(const char *__dir_path, int (*__callback)(const char *, const struct stat *, int, struct FTW *), int __max_fd_count, int __flags);
int nftw64(const char *__dir_path, int (*__callback)(const char *, const struct stat64 *, int, struct FTW *), int __max_fd_count, int __flags);
int nice(int __incr);
char *nl_langinfo(nl_item __item);
char *nl_langinfo_l(nl_item __item, locale_t __l);
long nrand48(unsigned short *__xsubi);
unsigned int ntohl(unsigned int __x);
unsigned short ntohs(unsigned short __x);
int open(const char *__path, int __flags, ...);
int open64(const char *__path, int __flags, ...);
FILE *open_memstream(char **__ptr, size_t *__size_ptr);
FILE *open_wmemstream(unsigned int **__ptr, size_t *__size_ptr);
int openat(int __dir_fd, const char *__path, int __flags, ...);
int openat64(int __dir_fd, const char *__path, int __flags, ...);
DIR *opendir(const char *__path);
void openlog(const char *__prefix, int __option, int __facility);
long pathconf(const char *__path, int __name);
int pause(void);
int pclose(FILE *__fp);
void perror(const char *__msg);
int pipe(int *__fds);
int pipe2(int *__fds, int __flags);
int poll(struct pollfd *__fds, nfds_t __count, int __timeout_ms);
FILE *popen(const char *__command, const char *__mode);
int posix_fadvise(int __fd, off_t __offset, off_t __length, int __advice);
int posix_fadvise64(int __fd, off64_t __offset, off64_t __length, int __advice);
int posix_fallocate(int __fd, off_t __offset, off_t __length);
int posix_fallocate64(int __fd, off64_t __offset, off64_t __length);
int posix_madvise(void *__addr, size_t __size, int __advice);
int posix_memalign(void **__memptr, size_t __alignment, size_t __size);
int posix_openpt(int __flags);
int posix_spawn(pid_t *__pid, const char *__path, const posix_spawn_file_actions_t *__actions, const posix_spawnattr_t *__attr, char *const *__argv, char *const *__env);
int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *__actions, const char *__path);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *__actions, int __fd);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *__actions, int __fd, int __new_fd);
int posix_spawn_file_actions_addfchdir_np(posix_spawn_file_actions_t *__actions, int __fd);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *__actions, int __fd, const char *__path, int __flags, mode_t __mode);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *__actions);
int posix_spawn_file_actions_init(posix_spawn_file_actions_t *__actions);
int posix_spawnattr_destroy(posix_spawnattr_t *__attr);
int posix_spawnattr_getflags(const posix_spawnattr_t *__attr, short *__flags);
int posix_spawnattr_getpgroup(const posix_spawnattr_t *__attr, pid_t *__pgroup);
int posix_spawnattr_getschedparam(const posix_spawnattr_t *__attr, struct sched_param *__param);
int posix_spawnattr_getschedpolicy(const posix_spawnattr_t *__attr, int *__policy);
int posix_spawnattr_getsigdefault(const posix_spawnattr_t *__attr, sigset_t *__mask);
int posix_spawnattr_getsigdefault64(const posix_spawnattr_t *__attr, sigset64_t *__mask);
int posix_spawnattr_getsigmask(const posix_spawnattr_t *__attr, sigset_t *__mask);
int posix_spawnattr_getsigmask64(const posix_spawnattr_t *__attr, sigset64_t *__mask);
int posix_spawnattr_init(posix_spawnattr_t *__attr);
int posix_spawnattr_setflags(posix_spawnattr_t *__attr, short __flags);
int posix_spawnattr_setpgroup(posix_spawnattr_t *__attr, pid_t __pgroup);
int posix_spawnattr_setschedparam(posix_spawnattr_t *__attr, const struct sched_param *__param);
int posix_spawnattr_setschedpolicy(posix_spawnattr_t *__attr, int __policy);
int posix_spawnattr_setsigdefault(posix_spawnattr_t *__attr, const sigset_t *__mask);
int posix_spawnattr_setsigdefault64(posix_spawnattr_t *__attr, const sigset64_t *__mask);
int posix_spawnattr_setsigmask(posix_spawnattr_t *__attr, const sigset_t *__mask);
int posix_spawnattr_setsigmask64(posix_spawnattr_t *__attr, const sigset64_t *__mask);
int posix_spawnp(pid_t *__pid, const char *__file, const posix_spawn_file_actions_t *__actions, const posix_spawnattr_t *__attr, char *const *__argv, char *const *__env);
double pow(double __x, double __y);
float powf(float __x, float __y);
long double powl(long double __x, long double __y);
int ppoll(struct pollfd *__fds, nfds_t __count, const struct timespec *__timeout, const sigset_t *__mask);
int ppoll64(struct pollfd *__fds, nfds_t __count, const struct timespec *__timeout, const sigset64_t *__mask);
int prctl(int __op, ...);
ssize_t pread(int __fd, void *__buf, size_t __count, off_t __offset);
ssize_t pread64(int __fd, void *__buf, size_t __count, off64_t __offset);
ssize_t preadv(int __fd, const struct iovec *__iov, int __count, off_t __offset);
ssize_t preadv2(int __fd, const struct iovec *__iov, int __count, off_t __offset, int __flags);
ssize_t preadv64(int __fd, const struct iovec *__iov, int __count, off64_t __offset);
ssize_t preadv64v2(int __fd, const struct iovec *__iov, int __count, off64_t __offset, int __flags);
int printf(const char *__fmt, ...);
int prlimit(pid_t __pid, int __resource, const struct rlimit *__new_limit, struct rlimit *__old_limit);
int prlimit64(pid_t __pid, int __resource, const struct rlimit64 *__new_limit, struct rlimit64 *__old_limit);
ssize_t process_madvise(int __pid_fd, const struct iovec *__iov, size_t __count, int __advice, unsigned int __flags);
ssize_t process_vm_readv(pid_t __pid, const struct iovec *__local_iov, unsigned long __local_iov_count, const struct iovec *__remote_iov, unsigned long __remote_iov_count, unsigned long __flags);
ssize_t process_vm_writev(pid_t __pid, const struct iovec *__local_iov, unsigned long __local_iov_count, const struct iovec *__remote_iov, unsigned long __remote_iov_count, unsigned long __flags);
int pselect(int __max_fd_plus_one, fd_set *__read_fds, fd_set *__write_fds, fd_set *__exception_fds, const struct timespec *__timeout, const sigset_t *__mask);
int pselect64(int __max_fd_plus_one, fd_set *__read_fds, fd_set *__write_fds, fd_set *__exception_fds, const struct timespec *__timeout, const sigset64_t *__mask);
void psiginfo(const siginfo_t *__info, const char *__msg);
void psignal(int __signal, const char *__msg);
int pthread_atfork(void (*__prepare)(void), void (*__parent)(void), void (*__child)(void));
int pthread_attr_destroy(pthread_attr_t *__attr);
int pthread_attr_getdetachstate(const pthread_attr_t *__attr, int *__state);
int pthread_attr_getguardsize(const pthread_attr_t *__attr, size_t *__size);
int pthread_attr_getinheritsched(const pthread_attr_t *__attr, int *__flag);
int pthread_attr_getschedparam(const pthread_attr_t *__attr, struct sched_param *__param);
int pthread_attr_getschedpolicy(const pthread_attr_t *__attr, int *__policy);
int pthread_attr_getscope(const pthread_attr_t *__attr, int *__scope);
int pthread_attr_getstack(const pthread_attr_t *__attr, void **__addr, size_t *__size);
int pthread_attr_getstacksize(const pthread_attr_t *__attr, size_t *__size);
int pthread_attr_init(pthread_attr_t *__attr);
int pthread_attr_setdetachstate(pthread_attr_t *__attr, int __state);
int pthread_attr_setguardsize(pthread_attr_t *__attr, size_t __size);
int pthread_attr_setinheritsched(pthread_attr_t *__attr, int __flag);
int pthread_attr_setschedparam(pthread_attr_t *__attr, const struct sched_param *__param);
int pthread_attr_setschedpolicy(pthread_attr_t *__attr, int __policy);
int pthread_attr_setscope(pthread_attr_t *__attr, int __scope);
int pthread_attr_setstack(pthread_attr_t *__attr, void *__addr, size_t __size);
int pthread_attr_setstacksize(pthread_attr_t *__addr, size_t __size);
int pthread_barrier_destroy(pthread_barrier_t *__barrier);
int pthread_barrier_init(pthread_barrier_t *__barrier, const pthread_barrierattr_t *__attr, unsigned int __count);
int pthread_barrier_wait(pthread_barrier_t *__barrier);
int pthread_barrierattr_destroy(pthread_barrierattr_t *__attr);
int pthread_barrierattr_getpshared(const pthread_barrierattr_t *__attr, int *__shared);
int pthread_barrierattr_init(pthread_barrierattr_t *__attr);
int pthread_barrierattr_setpshared(pthread_barrierattr_t *__attr, int __shared);
int pthread_cond_broadcast(pthread_cond_t *__cond);
int pthread_cond_clockwait(pthread_cond_t *__cond, pthread_mutex_t *__mutex, clockid_t __clock, const struct timespec *__timeout);
int pthread_cond_destroy(pthread_cond_t *__cond);
int pthread_cond_init(pthread_cond_t *__cond, const pthread_condattr_t *__attr);
int pthread_cond_signal(pthread_cond_t *__cond);
int pthread_cond_timedwait(pthread_cond_t *__cond, pthread_mutex_t *__mutex, const struct timespec *__timeout);
int pthread_cond_timedwait_monotonic_np(pthread_cond_t *__cond, pthread_mutex_t *__mutex, const struct timespec *__timeout);
int pthread_cond_wait(pthread_cond_t *__cond, pthread_mutex_t *__mutex);
int pthread_condattr_destroy(pthread_condattr_t *__attr);
int pthread_condattr_getclock(const pthread_condattr_t *__attr, clockid_t *__clock);
int pthread_condattr_getpshared(const pthread_condattr_t *__attr, int *__shared);
int pthread_condattr_init(pthread_condattr_t *__attr);
int pthread_condattr_setclock(pthread_condattr_t *__attr, clockid_t __clock);
int pthread_condattr_setpshared(pthread_condattr_t *__attr, int __shared);
int pthread_create(pthread_t *__pthread_ptr, const pthread_attr_t *__attr, void *(*__start_routine)(void *), void *);
int pthread_detach(pthread_t __pthread);
int pthread_equal(pthread_t __lhs, pthread_t __rhs);
_Noreturn void pthread_exit(void *__return_value);
int pthread_getattr_np(pthread_t __pthread, pthread_attr_t *__attr);
int pthread_getcpuclockid(pthread_t __pthread, clockid_t *__clock);
int pthread_getname_np(pthread_t __pthread, char *__buf, size_t __n);
int pthread_getschedparam(pthread_t __pthread, int *__policy, struct sched_param *__param);
void *pthread_getspecific(pthread_key_t __key);
pid_t pthread_gettid_np(pthread_t __pthread);
int pthread_join(pthread_t __pthread, void **__return_value_ptr);
int pthread_key_create(pthread_key_t *__key_ptr, void (*__key_destructor)(void *));
int pthread_key_delete(pthread_key_t __key);
int pthread_kill(pthread_t __pthread, int __signal);
int pthread_mutex_clocklock(pthread_mutex_t *__mutex, clockid_t __clock, const struct timespec *__abstime);
int pthread_mutex_destroy(pthread_mutex_t *__mutex);
int pthread_mutex_init(pthread_mutex_t *__mutex, const pthread_mutexattr_t *__attr);
int pthread_mutex_lock(pthread_mutex_t *__mutex);
int pthread_mutex_timedlock(pthread_mutex_t *__mutex, const struct timespec *__timeout);
int pthread_mutex_timedlock_monotonic_np(pthread_mutex_t *__mutex, const struct timespec *__timeout);
int pthread_mutex_trylock(pthread_mutex_t *__mutex);
int pthread_mutex_unlock(pthread_mutex_t *__mutex);
int pthread_mutexattr_destroy(pthread_mutexattr_t *__attr);
int pthread_mutexattr_getprotocol(const pthread_mutexattr_t *__attr, int *__protocol);
int pthread_mutexattr_getpshared(const pthread_mutexattr_t *__attr, int *__shared);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *__attr, int *__type);
int pthread_mutexattr_init(pthread_mutexattr_t *__attr);
int pthread_mutexattr_setprotocol(pthread_mutexattr_t *__attr, int __protocol);
int pthread_mutexattr_setpshared(pthread_mutexattr_t *__attr, int __shared);
int pthread_mutexattr_settype(pthread_mutexattr_t *__attr, int __type);
int pthread_once(pthread_once_t *__once, void (*__init_routine)(void));
int pthread_rwlock_clockrdlock(pthread_rwlock_t *__rwlock, clockid_t __clock, const struct timespec *__timeout);
int pthread_rwlock_clockwrlock(pthread_rwlock_t *__rwlock, clockid_t __clock, const struct timespec *__timeout);
int pthread_rwlock_destroy(pthread_rwlock_t *__rwlock);
int pthread_rwlock_init(pthread_rwlock_t *__rwlock, const pthread_rwlockattr_t *__attr);
int pthread_rwlock_rdlock(pthread_rwlock_t *__rwlock);
int pthread_rwlock_timedrdlock(pthread_rwlock_t *__rwlock, const struct timespec *__timeout);
int pthread_rwlock_timedrdlock_monotonic_np(pthread_rwlock_t *__rwlock, const struct timespec *__timeout);
int pthread_rwlock_timedwrlock(pthread_rwlock_t *__rwlock, const struct timespec *__timeout);
int pthread_rwlock_timedwrlock_monotonic_np(pthread_rwlock_t *__rwlock, const struct timespec *__timeout);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *__rwlock);
int pthread_rwlock_trywrlock(pthread_rwlock_t *__rwlock);
int pthread_rwlock_unlock(pthread_rwlock_t *__rwlock);
int pthread_rwlock_wrlock(pthread_rwlock_t *__rwlock);
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *__attr);
int pthread_rwlockattr_getkind_np(const pthread_rwlockattr_t *__attr, int *__kind);
int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *__attr, int *__shared);
int pthread_rwlockattr_init(pthread_rwlockattr_t *__attr);
int pthread_rwlockattr_setkind_np(pthread_rwlockattr_t *__attr, int __kind);
int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *__attr, int __shared);
pthread_t pthread_self(void);
int pthread_setname_np(pthread_t __pthread, const char *__name);
int pthread_setschedparam(pthread_t __pthread, int __policy, const struct sched_param *__param);
int pthread_setschedprio(pthread_t __pthread, int __priority);
int pthread_setspecific(pthread_key_t __key, const void *__value);
int pthread_sigmask(int __how, const sigset_t *__new_set, sigset_t *__old_set);
int pthread_sigmask64(int __how, const sigset64_t *__new_set, sigset64_t *__old_set);
int pthread_sigqueue(pthread_t __pthread, int __signal, const union sigval __value);
int pthread_spin_destroy(pthread_spinlock_t *__spinlock);
int pthread_spin_init(pthread_spinlock_t *__spinlock, int __shared);
int pthread_spin_lock(pthread_spinlock_t *__spinlock);
int pthread_spin_trylock(pthread_spinlock_t *__spinlock);
int pthread_spin_unlock(pthread_spinlock_t *__spinlock);
long ptrace(int __op, ...);
char *ptsname(int __fd);
int ptsname_r(int __fd, char *__buf, size_t __n);
int putc(int __ch, FILE *__fp);
int putc_unlocked(int __ch, FILE *__fp);
int putchar(int __ch);
int putchar_unlocked(int __ch);
int putenv(char *__assignment);
int puts(const char *__s);
struct utmp *pututline(const struct utmp *__entry);
wint_t putwc(unsigned int __wc, FILE *__fp);
wint_t putwchar(unsigned int __wc);
ssize_t pwrite(int __fd, const void *__buf, size_t __count, off_t __offset);
ssize_t pwrite64(int __fd, const void *__buf, size_t __count, off64_t __offset);
ssize_t pwritev(int __fd, const struct iovec *__iov, int __count, off_t __offset);
ssize_t pwritev2(int __fd, const struct iovec *__iov, int __count, off_t __offset, int __flags);
ssize_t pwritev64(int __fd, const struct iovec *__iov, int __count, off64_t __offset);
ssize_t pwritev64v2(int __fd, const struct iovec *__iov, int __count, off64_t __offset, int __flags);
void qsort(void *__array, size_t __n, size_t __size, int (*__comparator)(const void *__lhs, const void *__rhs));
_Noreturn void quick_exit(int __status);
int raise(int __signal);
int rand(void);
int rand_r(unsigned int *__seed_ptr);
long random(void);
ssize_t read(int __fd, void *__buf, size_t __count);
ssize_t readahead(int __fd, off64_t __offset, size_t __length);
struct dirent *readdir(DIR *__dir);
struct dirent64 *readdir64(DIR *__dir);
int readdir64_r(DIR *__dir, struct dirent64 *__entry, struct dirent64 **__buffer);
int readdir_r(DIR *__dir, struct dirent *__entry, struct dirent **__buffer);
ssize_t readlink(const char *__path, char *__buf, size_t __buf_size);
ssize_t readlinkat(int __dir_fd, const char *__path, char *__buf, size_t __buf_size);
ssize_t readv(int __fd, const struct iovec *__iov, int __count);
void *realloc(void *__ptr, size_t __byte_count);
void *reallocarray(void *__ptr, size_t __item_count, size_t __item_size);
char *realpath(const char *__path, char *__resolved);
ssize_t recv(int __fd, void *__buf, size_t __n, int __flags);
ssize_t recvfrom(int __fd, void *__buf, size_t __n, int __flags, struct sockaddr *__src_addr, socklen_t *__src_addr_length);
int recvmmsg(int __fd, struct mmsghdr *__msgs, unsigned int __msg_count, int __flags, const struct timespec *__timeout);
ssize_t recvmsg(int __fd, struct msghdr *__msg, int __flags);
int regcomp(regex_t *__re, const char *__regex, int __flags);
size_t regerror(int __error_code, const regex_t *__re, char *__buf, size_t __n);
int regexec(const regex_t *__re, const char *__s, size_t __match_count, regmatch_t *__matches, int __flags);
void regfree(regex_t *__re);
double remainder(double __x, double __y);
float remainderf(float __x, float __y);
long double remainderl(long double __x, long double __y);
int remove(const char *__path);
int removexattr(const char *__path, const char *__name);
void remque(void *__element);
double remquo(double __x, double __y, int *__quotient_bits);
float remquof(float __x, float __y, int *__quotient_bits);
long double remquol(long double __x, long double __y, int *__quotient_bits);
int rename(const char *__old_path, const char *__new_path);
int renameat(int __old_dir_fd, const char *__old_path, int __new_dir_fd, const char *__new_path);
int renameat2(int __old_dir_fd, const char *__old_path, int __new_dir_fd, const char *__new_path, unsigned int __flags);
void rewind(FILE *__fp);
void rewinddir(DIR *__dir);
double rint(double __x);
float rintf(float __x);
long double rintl(long double __x);
int rmdir(const char *__path);
double round(double __x);
float roundf(float __x);
long double roundl(long double __x);
void *sbrk(ptrdiff_t __increment);
double scalb(double __x, double __exponent);
float scalbf(float __x, float __exponent);
double scalbln(double __x, long __exponent);
float scalblnf(float __x, long __exponent);
long double scalblnl(long double __x, long __exponent);
double scalbn(double __x, int __exponent);
float scalbnf(float __x, int __exponent);
long double scalbnl(long double __x, int __exponent);
int scandir(const char *__path, struct dirent ***__name_list, int (*__filter)(const struct dirent *), int (*__comparator)(const struct dirent **, const struct dirent **));
int scandir64(const char *__path, struct dirent64 ***__name_list, int (*__filter)(const struct dirent64 *), int (*__comparator)(const struct dirent64 **, const struct dirent64 **));
int scandirat(int __dir_fd, const char *__path, struct dirent ***__name_list, int (*__filter)(const struct dirent *), int (*__comparator)(const struct dirent **, const struct dirent **));
int scandirat64(int __dir_fd, const char *__path, struct dirent64 ***__name_list, int (*__filter)(const struct dirent64 *), int (*__comparator)(const struct dirent64 **, const struct dirent64 **));
int scanf(const char *__fmt, ...);
int sched_get_priority_max(int __policy);
int sched_get_priority_min(int __policy);
int sched_getaffinity(pid_t __pid, size_t __set_size, cpu_set_t *__set);
int sched_getattr(pid_t __pid, struct sched_attr *__attr, unsigned int __size, unsigned int __flags);
int sched_getcpu(void);
int sched_getparam(pid_t __pid, struct sched_param *__param);
int sched_getscheduler(pid_t __pid);
int sched_rr_get_interval(pid_t __pid, struct timespec *__quantum);
int sched_setaffinity(pid_t __pid, size_t __set_size, const cpu_set_t *__set);
int sched_setattr(pid_t __pid, struct sched_attr *__attr, unsigned int __flags);
int sched_setparam(pid_t __pid, const struct sched_param *__param);
int sched_setscheduler(pid_t __pid, int __policy, const struct sched_param *__param);
int sched_yield(void);
unsigned short *seed48(unsigned short *__seed16v);
void seekdir(DIR *__dir, long __location);
int select(int __max_fd_plus_one, fd_set *__read_fds, fd_set *__write_fds, fd_set *__exception_fds, struct timeval *__timeout);
int sem_clockwait(sem_t *__sem, clockid_t __clock, const struct timespec *__ts);
int sem_close(sem_t *__sem);
int sem_destroy(sem_t *__sem);
int sem_getvalue(sem_t *__sem, int *__value);
int sem_init(sem_t *__sem, int __shared, unsigned int __value);
sem_t *sem_open(const char *__name, int _flags, ...);
int sem_post(sem_t *__sem);
int sem_timedwait(sem_t *__sem, const struct timespec *__ts);
int sem_timedwait_monotonic_np(sem_t *__sem, const struct timespec *__ts);
int sem_trywait(sem_t *__sem);
int sem_unlink(const char *__name);
int sem_wait(sem_t *__sem);
int semctl(int __sem_id, int __sem_num, int __op, ...);
int semget(key_t key, int __sem_count, int __flags);
int semop(int __sem_id, struct sembuf *__ops, size_t __op_count);
int semtimedop(int __sem_id, struct sembuf *__ops, size_t __op_count, const struct timespec *__timeout);
ssize_t send(int __fd, const void *__buf, size_t __n, int __flags);
ssize_t sendfile(int __out_fd, int __in_fd, off_t *__offset, size_t __count);
ssize_t sendfile64(int __out_fd, int __in_fd, off64_t *__offset, size_t __count);
int sendmmsg(int __fd, const struct mmsghdr *__msgs, unsigned int __msg_count, int __flags);
ssize_t sendmsg(int __fd, const struct msghdr *__msg, int __flags);
ssize_t sendto(int __fd, const void *__buf, size_t __n, int __flags, const struct sockaddr *__dst_addr, socklen_t __dst_addr_length);
void setbuf(FILE *__fp, char *__buf);
void setbuffer(FILE *__fp, char *__buf, int __size);
int setdomainname(const char *__name, size_t __n);
int setegid(gid_t __gid);
int setenv(const char *__name, const char *__value, int __overwrite);
int seteuid(uid_t __uid);
int setgid(gid_t __gid);
void setgrent(void);
int setgroups(size_t __size, const gid_t *__list);
void sethostent(int __stay_open);
int sethostname(const char *__name, size_t __n);
int setitimer(int __which, const struct itimerval *__new_value, struct itimerval *__old_value);
int setjmp(jmp_buf __env);
int setlinebuf(FILE *__fp);
char *setlocale(int __category, const char *__locale_name);
int setlogmask(int __mask);
void setnetent(int __stay_open);
int setns(int __fd, int __ns_type);
int setpgid(pid_t __pid, pid_t __pgid);
int setpgrp(void);
int setpriority(int __which, id_t __who, int __priority);
void setprogname(const char *__name);
void setprotoent(int __stay_open);
void setpwent(void);
int setregid(gid_t __rgid, gid_t __egid);
int setresgid(gid_t __rgid, gid_t __egid, gid_t __sgid);
int setresuid(uid_t __ruid, uid_t __euid, uid_t __suid);
int setreuid(uid_t __ruid, uid_t __euid);
int setrlimit(int __resource, const struct rlimit *__limit);
int setrlimit64(int __resource, const struct rlimit64 *__limit);
void setservent(int __stay_open);
pid_t setsid(void);
int setsockopt(int __fd, int __level, int __option, const void *__value, socklen_t __value_length);
char *setstate(char *__state);
int settimeofday(const struct timeval *__tv, const struct timezone *__tz);
int setuid(uid_t __uid);
void setutent(void);
int setvbuf(FILE *__fp, char *__buf, int __mode, size_t __size);
int setxattr(const char *__path, const char *__name, const void *__value, size_t __size, int __flags);
void *shmat(int __shm_id, const void *__addr, int __flags);
int shmctl(int __shm_id, int __op, struct shmid64_ds *__buf);
int shmdt(const void *__addr);
int shmget(key_t key, size_t __size, int __flags);
int shutdown(int __fd, int __how);
int sigaction(int __signal, const struct sigaction *__new_action, struct sigaction *__old_action);
int sigaction64(int __signal, const struct sigaction64 *__new_action, struct sigaction64 *__old_action);
int sigaddset(sigset_t *__set, int __signal);
int sigaddset64(sigset64_t *__set, int __signal);
int sigaltstack(const stack_t *__new_signal_stack, stack_t *__old_signal_stack);
int sigdelset(sigset_t *__set, int __signal);
int sigdelset64(sigset64_t *__set, int __signal);
int sigemptyset(sigset_t *__set);
int sigemptyset64(sigset64_t *__set);
int sigfillset(sigset_t *__set);
int sigfillset64(sigset64_t *__set);
int sighold(int __signal);
int sigignore(int __signal);
int siginterrupt(int __signal, int __flag);
int sigismember(const sigset_t *__set, int __signal);
int sigismember64(const sigset64_t *__set, int __signal);
_Noreturn void siglongjmp(sigjmp_buf __env, int __value);
sighandler_t signal(int __signal, sighandler_t __handler);
int signalfd(int __fd, const sigset_t *__mask, int __flags);
int signalfd64(int __fd, const sigset64_t *__mask, int __flags);
double significand(double __x);
float significandf(float __x);
long double significandl(long double __x);
int sigpause(int __signal);
int sigpending(sigset_t *__set);
int sigpending64(sigset64_t *__set);
int sigprocmask(int __how, const sigset_t *__new_set, sigset_t *__old_set);
int sigprocmask64(int __how, const sigset64_t *__new_set, sigset64_t *__old_set);
int sigqueue(pid_t __pid, int __signal, const union sigval __value);
int sigrelse(int __signal);
sighandler_t sigset(int __signal, sighandler_t __handler);
int sigsetjmp(sigjmp_buf __env, int __save_signal_mask);
int sigsuspend(const sigset_t *__mask);
int sigsuspend64(const sigset64_t *__mask);
int sigtimedwait(const sigset_t *__set, siginfo_t *__info, const struct timespec *__timeout);
int sigtimedwait64(const sigset64_t *__set, siginfo_t *__info, const struct timespec *__timeout);
int sigwait(const sigset_t *__set, int *__signal);
int sigwait64(const sigset64_t *__set, int *__signal);
int sigwaitinfo(const sigset_t *__set, siginfo_t *__info);
int sigwaitinfo64(const sigset64_t *__set, siginfo_t *__info);
double sin(double __x);
void sincos(double __x, double *__sin, double *__cos);
void sincosf(float __x, float *__sin, float *__cos);
void sincosl(long double __x, long double *__sin, long double *__cos);
float sinf(float __x);
double sinh(double __x);
float sinhf(float __x);
long double sinhl(long double __x);
long double sinl(long double __x);
unsigned int sleep(unsigned int __seconds);
int snprintf(char *__buf, size_t __size, const char *__fmt, ...);
int socket(int __af, int __type, int __protocol);
int socketpair(int __af, int __type, int __protocol, int *__fds);
ssize_t splice(int __in_fd, off64_t *__in_offset, int __out_fd, off64_t *__out_offset, size_t __length, unsigned int __flags);
int sprintf(char *__s, const char *__fmt, ...);
double sqrt(double __x);
float sqrtf(float __x);
long double sqrtl(long double __x);
void srand(unsigned int __seed);
void srand48(long __seed);
void srandom(unsigned int __seed);
int sscanf(const char *__s, const char *__fmt, ...);
int stat(const char *__path, struct stat *__buf);
int stat64(const char *__path, struct stat64 *__buf);
int statvfs(const char *__path, struct statvfs *__buf);
int statvfs64(const char *__path, struct statvfs64 *__buf);
int statx(int __dir_fd, const char *__path, int __flags, unsigned int __mask, struct statx *__buf);
char *stpcpy(char *__dst, const char *__src);
char *stpncpy(char *__dst, const char *__src, size_t __n);
int strcasecmp(const char *__s1, const char *__s2);
int strcasecmp_l(const char *__s1, const char *__s2, locale_t __l);
char *strcasestr(const char *__haystack, const char *__needle);
char *strcat(char *__dst, const char *__src);
char *strchr(const char *__s, int __ch);
char *strchrnul(const char *__s, int __ch);
int strcmp(const char *__lhs, const char *__rhs);
int strcoll(const char *__lhs, const char *__rhs);
int strcoll_l(const char *__lhs, const char *__rhs, locale_t __l);
char *strcpy(char *__dst, const char *__src);
size_t strcspn(const char *__s, const char *__reject);
char *strdup(const char *__s);
char *strerror(int __errno_value);
char *strerror_l(int __errno_value, locale_t __l);
char *strerror_r(int __errno_value, char *__buf, size_t __n);
const char *strerrordesc_np(int __errno_value);
const char *strerrorname_np(int __errno_value);
size_t strftime(char *__buf, size_t __n, const char *__fmt, const struct tm *__tm);
size_t strftime_l(char *__buf, size_t __n, const char *__fmt, const struct tm *__tm, locale_t __l);
size_t strlcat(char *__dst, const char *__src, size_t __n);
size_t strlcpy(char *__dst, const char *__src, size_t __n);
size_t strlen(const char *__s);
int strncasecmp(const char *__s1, const char *__s2, size_t __n);
int strncasecmp_l(const char *__s1, const char *__s2, size_t __n, locale_t __l);
char *strncat(char *__dst, const char *__src, size_t __n);
int strncmp(const char *__lhs, const char *__rhs, size_t __n);
char *strncpy(char *__dst, const char *__src, size_t __n);
char *strndup(const char *__s, size_t __n);
size_t strnlen(const char *__s, size_t __n);
char *strpbrk(const char *__s, const char *__accept);
char *strptime(const char *__s, const char *__fmt, struct tm *__tm);
char *strptime_l(const char *__s, const char *__fmt, struct tm *__tm, locale_t __l);
char *strrchr(const char *__s, int __ch);
char *strsep(char **__s_ptr, const char *__delimiter);
char *strsignal(int __signal);
size_t strspn(const char *__s, const char *__accept);
char *strstr(const char *__haystack, const char *__needle);
double strtod(const char *__s, char **__end_ptr);
double strtod_l(const char *__s, char **__end_ptr, locale_t __l);
float strtof(const char *__s, char **__end_ptr);
float strtof_l(const char *__s, char **__end_ptr, locale_t __l);
intmax_t strtoimax(const char *__s, char **__end_ptr, int __base);
char *strtok(char *__s, const char *__delimiter);
char *strtok_r(char *__s, const char *__delimiter, char **__pos_ptr);
long strtol(const char *__s, char **__end_ptr, int __base);
long strtol_l(const char *__s, char **__end_ptr, int, locale_t __l);
long double strtold(const char *__s, char **__end_ptr);
long double strtold_l(const char *__s, char **__end_ptr, locale_t __l);
long long strtoll(const char *__s, char **__end_ptr, int __base);
long long strtoll_l(const char *__s, char **__end_ptr, int __base, locale_t __l);
unsigned long strtoul(const char *__s, char **__end_ptr, int __base);
unsigned long strtoul_l(const char *__s, char **__end_ptr, int __base, locale_t __l);
unsigned long long strtoull(const char *__s, char **__end_ptr, int __base);
unsigned long long strtoull_l(const char *__s, char **__end_ptr, int __base, locale_t __l);
uintmax_t strtoumax(const char *__s, char **__end_ptr, int __base);
size_t strxfrm(char *__dst, const char *__src, size_t __n);
size_t strxfrm_l(char *__dst, const char *__src, size_t __n, locale_t __l);
void swab(const void *__src, void *__dst, ssize_t __byte_count);
int swprintf(unsigned int *__buf, size_t __n, const unsigned int *__fmt, ...);
int swscanf(const unsigned int *__s, const unsigned int *__fmt, ...);
int symlink(const char *__old_path, const char *__new_path);
int symlinkat(const char *__old_path, int __new_dir_fd, const char *__new_path);
void sync(void);
int sync_file_range(int __fd, off64_t __offset, off64_t __length, unsigned int __flags);
int syncfs(int __fd);
long syscall(long __number, ...);
long sysconf(int __name);
int sysinfo(struct sysinfo *__info);
void syslog(int __priority, const char *__fmt, ...);
int system(const char *__command);
double tan(double __x);
float tanf(float __x);
double tanh(double __x);
float tanhf(float __x);
long double tanhl(long double __x);
long double tanl(long double __x);
int tcdrain(int __fd);
int tcflow(int __fd, int __action);
int tcflush(int __fd, int __queue);
int tcgetattr(int __fd, struct termios *__t);
pid_t tcgetpgrp(int __fd);
pid_t tcgetsid(int __fd);
int tcgetwinsize(int __fd, struct winsize *__size);
int tcsendbreak(int __fd, int __duration);
int tcsetattr(int __fd, int __optional_actions, const struct termios *__t);
int tcsetpgrp(int __fd, pid_t __pid);
int tcsetwinsize(int __fd, const struct winsize *__size);
void *tdelete(const void *__key, void **__root_ptr, int (*__comparator)(const void *, const void *));
void tdestroy(void *__root, void (*__free_fn)(void *));
ssize_t tee(int __in_fd, int __out_fd, size_t __length, unsigned int __flags);
long telldir(DIR *__dir);
char *tempnam(const char *__dir, const char *__prefix);
void *tfind(const void *__key, void *const *__root_ptr, int (*__comparator)(const void *, const void *));
double tgamma(double __x);
float tgammaf(float __x);
long double tgammal(long double __x);
int tgkill(int __tgid, int __tid, int __signal);
time_t time(time_t *__t);
time_t timegm(struct tm *__tm);
time_t timelocal(struct tm *__tm);
int timer_create(clockid_t __clock, struct sigevent *__event, timer_t *__timer_ptr);
int timer_delete(timer_t __timer);
int timer_getoverrun(timer_t __timer);
int timer_gettime(timer_t _timer, struct itimerspec *__ts);
int timer_settime(timer_t __timer, int __flags, const struct itimerspec *__new_value, struct itimerspec *__old_value);
int timerfd_create(clockid_t __clock, int __flags);
int timerfd_gettime(int __fd, struct itimerspec *__current_value);
int timerfd_settime(int __fd, int __flags, const struct itimerspec *__new_value, struct itimerspec *__old_value);
clock_t times(struct tms *__buf);
int timespec_get(struct timespec *__ts, int __base);
int timespec_getres(struct timespec *__ts, int __base);
FILE *tmpfile(void);
FILE *tmpfile64(void);
char *tmpnam(char *__s);
int toascii(int __ch);
int tolower(int __ch);
int tolower_l(int __ch, locale_t __l);
int toupper(int __ch);
int toupper_l(int __ch, locale_t __l);
wint_t towctrans(wint_t __wc, wctrans_t __transform);
wint_t towctrans_l(wint_t __wc, wctrans_t __transform, locale_t __l);
wint_t towlower(wint_t __wc);
wint_t towlower_l(wint_t __wc, locale_t __l);
wint_t towupper(wint_t __wc);
wint_t towupper_l(wint_t __wc, locale_t __l);
double trunc(double __x);
int truncate(const char *__path, off_t __length);
int truncate64(const char *__path, off64_t __length);
float truncf(float __x);
long double truncl(long double __x);
void *tsearch(const void *__key, void **__root_ptr, int (*__comparator)(const void *, const void *));
char *ttyname(int __fd);
int ttyname_r(int __fd, char *__buf, size_t __buf_size);
void twalk(const void *__root, void (*__visitor)(const void *, VISIT, int));
timezone_t tzalloc(const char *__id);
void tzfree(timezone_t __tz);
void tzset(void);
mode_t umask(mode_t __mask);
int uname(struct utsname *__buf);
int ungetc(int __ch, FILE *__fp);
wint_t ungetwc(wint_t __wc, FILE *__fp);
int unlink(const char *__path);
int unlinkat(int __dirfd, const char *__path, int __flags);
int unlockpt(int __fd);
int unsetenv(const char *__name);
int unshare(int __flags);
locale_t uselocale(locale_t __l);
int usleep(useconds_t __microseconds);
int utime(const char *__filename, const struct utimbuf *__times);
int utimensat(int __dir_fd, const char *__path, const struct timespec *__times, int __flags);
int utimes(const char *__path, const struct timeval *__times);
int utmpname(const char *__path);
int vasprintf(char **__s_ptr, const char *__fmt, char *__args);
int vdprintf(int __fd, const char *__fmt, char *__args);
_Noreturn void verr(int __status, const char *__fmt, char *__args);
_Noreturn void verrx(int __status, const char *__fmt, char *__args);
pid_t vfork(void);
int vfprintf(FILE *__fp, const char *__fmt, char *__args);
int vfscanf(FILE *__fp, const char *__fmt, char *__args);
int vfwprintf(FILE *__fp, const unsigned int *__fmt, char *__args);
int vfwscanf(FILE *__fp, const unsigned int *__fmt, char *__args);
ssize_t vmsplice(int __fd, const struct iovec *__iov, size_t __count, unsigned int __flags);
int vprintf(const char *__fp, char *__args);
int vscanf(const char *__fmt, char *__args);
int vsnprintf(char *__buf, size_t __size, const char *__fmt, char *__args);
int vsprintf(char *__s, const char *__fmt, char *__args);
int vsscanf(const char *__s, const char *__fmt, char *__args);
int vswprintf(unsigned int *__buf, size_t __n, const unsigned int *__fmt, char *__args);
int vswscanf(const unsigned int *__s, const unsigned int *__fmt, char *__args);
void vsyslog(int __priority, const char *__fmt, char *__args);
void vwarn(const char *__fmt, char *__args);
void vwarnx(const char *__fmt, char *__args);
int vwprintf(const unsigned int *__fmt, char *__args);
int vwscanf(const unsigned int *__fmt, char *__args);
pid_t wait(int *__status);
pid_t wait4(pid_t __pid, int *__status, int __options, struct rusage *__rusage);
int waitid(idtype_t __type, id_t __id, siginfo_t *__info, int __options);
pid_t waitpid(pid_t __pid, int *__status, int __options);
void warn(const char *__fmt, ...);
void warnx(const char *__fmt, ...);
unsigned int *wcpcpy(unsigned int *__dst, const unsigned int *__src);
unsigned int *wcpncpy(unsigned int *__dst, const unsigned int *__src, size_t __n);
size_t wcrtomb(char *__buf, unsigned int __wc, mbstate_t *__ps);
int wcscasecmp(const unsigned int *__lhs, const unsigned int *__rhs);
int wcscasecmp_l(const unsigned int *__lhs, const unsigned int *__rhs, locale_t __l);
unsigned int *wcscat(unsigned int *__dst, const unsigned int *__src);
unsigned int *wcschr(const unsigned int *__s, unsigned int __wc);
int wcscmp(const unsigned int *__lhs, const unsigned int *__rhs);
int wcscoll(const unsigned int *__lhs, const unsigned int *__rhs);
int wcscoll_l(const unsigned int *__lhs, const unsigned int *__rhs, locale_t __l);
unsigned int *wcscpy(unsigned int *__dst, const unsigned int *__src);
size_t wcscspn(const unsigned int *__s, const unsigned int *__accept);
unsigned int *wcsdup(const unsigned int *__s);
size_t wcsftime(unsigned int *__buf, size_t __n, const unsigned int *__fmt, const struct tm *__tm);
size_t wcsftime_l(unsigned int *__buf, size_t __n, const unsigned int *__fmt, const struct tm *__tm, locale_t __l);
size_t wcslcat(unsigned int *__dst, const unsigned int *__src, size_t __n);
size_t wcslcpy(unsigned int *__dst, const unsigned int *__src, size_t __n);
size_t wcslen(const unsigned int *__s);
int wcsncasecmp(const unsigned int *__lhs, const unsigned int *__rhs, size_t __n);
int wcsncasecmp_l(const unsigned int *__lhs, const unsigned int *__rhs, size_t __n, locale_t __l);
unsigned int *wcsncat(unsigned int *__dst, const unsigned int *__src, size_t __n);
int wcsncmp(const unsigned int *__lhs, const unsigned int *__rhs, size_t __n);
unsigned int *wcsncpy(unsigned int *__dst, const unsigned int *__src, size_t __n);
size_t wcsnlen(const unsigned int *__s, size_t __n);
size_t wcsnrtombs(char *__dst, const unsigned int **__src, size_t __src_n, size_t __dst_n, mbstate_t *__ps);
unsigned int *wcspbrk(const unsigned int *__s, const unsigned int *__accept);
unsigned int *wcsrchr(const unsigned int *__s, unsigned int __wc);
size_t wcsrtombs(char *__dst, const unsigned int **__src, size_t __dst_n, mbstate_t *__ps);
size_t wcsrtombs_l(char *__dst, const unsigned int **__src, size_t __dst_n, mbstate_t *__ps, locale_t __l);
size_t wcsspn(const unsigned int *__s, const unsigned int *__accept);
unsigned int *wcsstr(const unsigned int *__haystack, const unsigned int *__needle);
double wcstod(const unsigned int *__s, unsigned int **__end_ptr);
double wcstod_l(const unsigned int *__s, unsigned int **__end_ptr, locale_t __l);
float wcstof(const unsigned int *__s, unsigned int **__end_ptr);
float wcstof_l(const unsigned int *__s, unsigned int **__end_ptr, locale_t __l);
intmax_t wcstoimax(const unsigned int *__s, unsigned int **__end_ptr, int __base);
unsigned int *wcstok(unsigned int *__s, const unsigned int *__delimiter, unsigned int **__ptr);
long wcstol(const unsigned int *__s, unsigned int **__end_ptr, int __base);
long wcstol_l(const unsigned int *__s, unsigned int **__end_ptr, int __base, locale_t __l);
long double wcstold(const unsigned int *__s, unsigned int **__end_ptr);
long double wcstold_l(const unsigned int *__s, unsigned int **__end_ptr, locale_t __l);
long long wcstoll(const unsigned int *__s, unsigned int **__end_ptr, int __base);
long long wcstoll_l(const unsigned int *__s, unsigned int **__end_ptr, int __base, locale_t __l);
size_t wcstombs(char *__dst, const unsigned int *__src, size_t __n);
unsigned long wcstoul(const unsigned int *__s, unsigned int **__end_ptr, int __base);
unsigned long wcstoul_l(const unsigned int *__s, unsigned int **__end_ptr, int __base, locale_t __l);
unsigned long long wcstoull(const unsigned int *__s, unsigned int **__end_ptr, int __base);
unsigned long long wcstoull_l(const unsigned int *__s, unsigned int **__end_ptr, int __base, locale_t __l);
uintmax_t wcstoumax(const unsigned int *__s, unsigned int **__end_ptr, int __base);
int wcswidth(const unsigned int *__s, size_t __n);
size_t wcsxfrm(unsigned int *__dst, const unsigned int *__src, size_t __n);
size_t wcsxfrm_l(unsigned int *__dst, const unsigned int *__src, size_t __n, locale_t __l);
int wctob(wint_t __wc);
int wctomb(char *__dst, unsigned int __wc);
wctrans_t wctrans(const char *__name);
wctrans_t wctrans_l(const char *__name, locale_t __l);
wctype_t wctype(const char *__name);
wctype_t wctype_l(const char *__name, locale_t __l);
int wcwidth(unsigned int __wc);
unsigned int *wmemchr(const unsigned int *__src, unsigned int __wc, size_t __n);
int wmemcmp(const unsigned int *__lhs, const unsigned int *__rhs, size_t __n);
unsigned int *wmemcpy(unsigned int *__dst, const unsigned int *__src, size_t __n);
unsigned int *wmemmove(unsigned int *__dst, const unsigned int *__src, size_t __n);
unsigned int *wmempcpy(unsigned int *__dst, const unsigned int *__src, size_t __n);
unsigned int *wmemset(unsigned int *__dst, unsigned int __wc, size_t __n);
int wprintf(const unsigned int *__fmt, ...);
ssize_t write(int __fd, const void *__buf, size_t __count);
ssize_t writev(int __fd, const struct iovec *__iov, int __count);
int wscanf(const unsigned int *__fmt, ...);
double y0(double __x);
float y0f(float __x);
double y1(double __x);
float y1f(float __x);
double yn(int __n, double __x);
float ynf(int __n, float __x);
