// The Linux types and prototypes for x86 (ILP32), as a program declares them
// with the GNU C Library's headers and _GNU_SOURCE.  Generated on 2026-10-17 by
// scripts/generate_linux_types.py --target linux-x86 from the headers of glibc
// 2.43: changed from them, as NeverD reads and renders their declarations
// again.  The headers' notices are in LICENSES/glibc-headers.txt.  Do not edit;
// generate it again.
// 589 types, 2956 prototypes, rendered by NeverD.
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

#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif

static_assert(sizeof(long) == 4, "these types need a 32-bit long");

struct __fsid_t;
struct __mbstate_t;
struct _G_fpos_t;
struct _G_fpos64_t;
struct _IO_FILE;
struct _IO_marker;
struct _IO_codecvt;
struct _IO_wide_data;
struct _IO_cookie_io_functions_t;
struct obstack;
struct div_t;
struct ldiv_t;
struct lldiv_t;
struct __locale_data;
struct __locale_struct;
struct __sigset_t;
struct timeval;
struct timespec;
struct fd_set;
union __atomic_wide_counter;
struct __pthread_internal_list;
struct __pthread_internal_slist;
struct __pthread_mutex_s;
struct __pthread_rwlock_arch_t;
struct __pthread_cond_s;
struct __once_flag;
union pthread_mutexattr_t;
union pthread_condattr_t;
union pthread_attr_t;
union pthread_mutex_t;
union pthread_cond_t;
union pthread_rwlock_t;
union pthread_rwlockattr_t;
union pthread_barrier_t;
union pthread_barrierattr_t;
struct random_data;
struct drand48_data;
struct flock;
struct flock64;
struct iovec;
struct f_owner_ex;
struct file_handle;
struct __kernel_fd_set;
struct __kernel_fsid_t;
struct open_how;
struct stat;
struct stat64;
struct tm;
struct lconv;
struct timex;
struct itimerspec;
struct sigevent;
union sigval;
struct siginfo_t;
struct sigaction;
struct _fpx_sw_bytes;
struct _fpreg;
struct _fpxreg;
struct _xmmreg;
struct _fpstate;
struct sigcontext;
struct _xsave_hdr;
struct _ymmh_state;
struct _xstate;
struct stack_t;
struct _libc_fpreg;
struct _libc_fpstate;
struct mcontext_t;
struct ucontext_t;
struct sigstack;
struct __jmp_buf_tag;
struct fenv_t;
struct femode_t;
struct imaxdiv_t;
struct dirent;
struct dirent64;
struct __dirstream;
struct Dl_info;
struct Dl_serpath;
struct Dl_serinfo;
struct link_map;
struct dl_find_object;
struct sched_attr;
struct sched_param;
struct cpu_set_t;
struct _pthread_cleanup_buffer;
struct __cancel_jmp_buf_tag;
struct __pthread_cleanup_frame;
union sem_t;
struct pollfd;
struct termios;
struct passwd;
struct group;
struct glob_t;
struct glob64_t;
struct re_dfa_t;
struct re_pattern_buffer;
struct re_registers;
struct regmatch_t;
struct option;
struct posix_spawnattr_t;
struct __spawn_action;
struct posix_spawn_file_actions_t;
struct Elf32_Ehdr;
struct Elf64_Ehdr;
struct Elf32_Shdr;
struct Elf64_Shdr;
struct Elf32_Chdr;
struct Elf64_Chdr;
struct Elf32_Sym;
struct Elf64_Sym;
struct Elf32_Syminfo;
struct Elf64_Syminfo;
struct Elf32_Rel;
struct Elf64_Rel;
struct Elf32_Rela;
struct Elf64_Rela;
struct Elf32_Phdr;
struct Elf64_Phdr;
struct Elf32_Dyn;
struct Elf64_Dyn;
struct Elf32_Verdef;
struct Elf64_Verdef;
struct Elf32_Verdaux;
struct Elf64_Verdaux;
struct Elf32_Verneed;
struct Elf64_Verneed;
struct Elf32_Vernaux;
struct Elf64_Vernaux;
struct Elf32_auxv_t;
struct Elf64_auxv_t;
struct Elf32_Nhdr;
struct Elf64_Nhdr;
struct Elf32_Move;
struct Elf64_Move;
union Elf32_gptab;
struct Elf32_RegInfo;
struct Elf_Options;
struct Elf_Options_Hw;
struct Elf32_Lib;
struct Elf64_Lib;
struct Elf_MIPS_ABIFlags_v0;
struct La_i86_regs;
struct La_i86_retval;
struct r_debug;
struct r_debug_extended;
struct dl_phdr_info;
struct sockaddr;
struct sockaddr_storage;
struct msghdr;
struct cmsghdr;
struct ucred;
struct linger;
struct osockaddr;
struct sockaddr_at;
struct sockaddr_ax25;
struct sockaddr_dl;
struct sockaddr_eon;
struct sockaddr_in;
struct sockaddr_in6;
struct sockaddr_inarp;
struct sockaddr_ipx;
struct sockaddr_iso;
struct sockaddr_ns;
struct sockaddr_un;
struct sockaddr_x25;
union __SOCKADDR_ARG;
union __CONST_SOCKADDR_ARG;
struct mmsghdr;
struct in_addr;
struct ip_opts;
struct in_pktinfo;
struct in6_addr;
struct ip_mreq;
struct ip_mreqn;
struct ip_mreq_source;
struct ipv6_mreq;
struct group_req;
struct group_source_req;
struct ip_msfilter;
struct group_filter;
struct in6_pktinfo;
struct ip6_mtuinfo;
struct rpcent;
struct netent;
struct hostent;
struct servent;
struct protoent;
struct addrinfo;
struct gaicb;
struct ifaddrs;
struct qelem;
struct entry;
struct _ENTRY;
struct hsearch_data;
struct utimbuf;
struct timezone;
struct itimerval;
struct lastlog;
struct exit_status;
struct utmp;
struct statx_timestamp;
struct statx;
struct statvfs;
struct statvfs64;
struct tms;
struct rusage;
struct winsize;
union epoll_data;
struct epoll_event;
struct epoll_params;
struct inotify_event;
struct rlimit;
struct rlimit64;
struct utsname;
struct prctl_mm_map;
struct __ptrace_peeksiginfo_args;
struct __ptrace_seccomp_metadata;
struct __ptrace_syscall_info;
struct __ptrace_rseq_configuration;
struct __ptrace_sud_config;
struct sysinfo;
struct signalfd_siginfo;
struct ipc_perm;
struct semid_ds;
struct __semid64_ds;
struct seminfo;
struct sembuf;
struct shmid_ds;
struct __shmid64_ds;
struct shminfo;
struct shm_info;
struct msqid_ds;
struct __msqid64_ds;
struct msginfo;
struct msgbuf;
struct tcphdr;
struct tcp_info;
struct tcp_md5sig;
struct tcp_diag_md5sig;
struct tcp_ao_add;
struct tcp_ao_del;
struct tcp_ao_info_opt;
struct tcp_ao_getsockopt;
struct tcp_ao_repair;
struct tcp_repair_opt;
struct tcp_cookie_transactions;
struct tcp_repair_window;
struct tcp_zerocopy_receive;
struct if_nameindex;
struct iface;
struct ifaddr;
struct ifmap;
struct ifreq;
struct ifconf;
struct aiocb;
struct aiocb64;
struct aioinit;
struct mq_attr;
struct mallinfo;
struct mallinfo2;
struct _ftsent;
struct FTS;
struct _ftsent64;
struct FTS64;
struct FTW;
struct wordexp_t;
struct spwd;

// __pid_type: 4 bytes
enum __pid_type
{
    F_OWNER_TID = 0,
    F_OWNER_PID = 1,
    F_OWNER_PGRP = 2,
    F_OWNER_GID = 2,
};

// reg_errcode_t: 4 bytes
enum reg_errcode_t
{
    _REG_ENOSYS = -1,
    _REG_NOERROR = 0,
    _REG_NOMATCH = 1,
    _REG_BADPAT = 2,
    _REG_ECOLLATE = 3,
    _REG_ECTYPE = 4,
    _REG_EESCAPE = 5,
    _REG_ESUBREG = 6,
    _REG_EBRACK = 7,
    _REG_EPAREN = 8,
    _REG_EBRACE = 9,
    _REG_BADBR = 10,
    _REG_ERANGE = 11,
    _REG_ESPACE = 12,
    _REG_BADRPT = 13,
    _REG_EEND = 14,
    _REG_ESIZE = 15,
    _REG_ERPAREN = 16,
};

// __socket_type: 4 bytes
enum __socket_type
{
    SOCK_STREAM = 0x1,
    SOCK_DGRAM = 0x2,
    SOCK_RAW = 0x3,
    SOCK_RDM = 0x4,
    SOCK_SEQPACKET = 0x5,
    SOCK_DCCP = 0x6,
    SOCK_PACKET = 0xA,
    SOCK_CLOEXEC = 0x80000,
    SOCK_NONBLOCK = 0x800,
};

// ACTION: 4 bytes
enum ACTION
{
    FIND = 0,
    ENTER = 1,
};

// VISIT: 4 bytes
enum VISIT
{
    preorder = 0,
    postorder = 1,
    endorder = 2,
    leaf = 3,
};

// __itimer_which: 4 bytes
enum __itimer_which
{
    ITIMER_REAL = 0,
    ITIMER_VIRTUAL = 1,
    ITIMER_PROF = 2,
};

// idtype_t: 4 bytes
enum idtype_t
{
    P_ALL = 0,
    P_PID = 1,
    P_PGID = 2,
    P_PIDFD = 3,
};

// EPOLL_EVENTS: 4 bytes
enum EPOLL_EVENTS
{
    EPOLLIN = 0x1,
    EPOLLPRI = 0x2,
    EPOLLOUT = 0x4,
    EPOLLRDNORM = 0x40,
    EPOLLRDBAND = 0x80,
    EPOLLWRNORM = 0x100,
    EPOLLWRBAND = 0x200,
    EPOLLMSG = 0x400,
    EPOLLERR = 0x8,
    EPOLLHUP = 0x10,
    EPOLLRDHUP = 0x2000,
    EPOLLEXCLUSIVE = 0x10000000,
    EPOLLWAKEUP = 0x20000000,
    EPOLLONESHOT = 0x40000000,
    EPOLLET = -2147483648,
};

// __rlimit_resource: 4 bytes
enum __rlimit_resource
{
    RLIMIT_CPU = 0,
    RLIMIT_FSIZE = 1,
    RLIMIT_DATA = 2,
    RLIMIT_STACK = 3,
    RLIMIT_CORE = 4,
    __RLIMIT_RSS = 5,
    RLIMIT_NOFILE = 7,
    __RLIMIT_OFILE = 7,
    RLIMIT_AS = 9,
    __RLIMIT_NPROC = 6,
    __RLIMIT_MEMLOCK = 8,
    __RLIMIT_LOCKS = 10,
    __RLIMIT_SIGPENDING = 11,
    __RLIMIT_MSGQUEUE = 12,
    __RLIMIT_NICE = 13,
    __RLIMIT_RTPRIO = 14,
    __RLIMIT_RTTIME = 15,
    __RLIMIT_NLIMITS = 16,
    __RLIM_NLIMITS = 16,
};

// __rusage_who: 4 bytes
enum __rusage_who
{
    RUSAGE_SELF = 0,
    RUSAGE_CHILDREN = -1,
    RUSAGE_THREAD = 1,
};

// __priority_which: 4 bytes
enum __priority_which
{
    PRIO_PROCESS = 0,
    PRIO_PGRP = 1,
    PRIO_USER = 2,
};

// __ptrace_request: 4 bytes
enum __ptrace_request
{
    PTRACE_TRACEME = 0x0,
    PTRACE_PEEKTEXT = 0x1,
    PTRACE_PEEKDATA = 0x2,
    PTRACE_PEEKUSER = 0x3,
    PTRACE_POKETEXT = 0x4,
    PTRACE_POKEDATA = 0x5,
    PTRACE_POKEUSER = 0x6,
    PTRACE_CONT = 0x7,
    PTRACE_KILL = 0x8,
    PTRACE_SINGLESTEP = 0x9,
    PTRACE_GETREGS = 0xC,
    PTRACE_SETREGS = 0xD,
    PTRACE_GETFPREGS = 0xE,
    PTRACE_SETFPREGS = 0xF,
    PTRACE_ATTACH = 0x10,
    PTRACE_DETACH = 0x11,
    PTRACE_GETFPXREGS = 0x12,
    PTRACE_SETFPXREGS = 0x13,
    PTRACE_SYSCALL = 0x18,
    PTRACE_GET_THREAD_AREA = 0x19,
    PTRACE_SET_THREAD_AREA = 0x1A,
    PTRACE_SYSEMU = 0x1F,
    PTRACE_SYSEMU_SINGLESTEP = 0x20,
    PTRACE_SINGLEBLOCK = 0x21,
    PTRACE_SETOPTIONS = 0x4200,
    PTRACE_GETEVENTMSG = 0x4201,
    PTRACE_GETSIGINFO = 0x4202,
    PTRACE_SETSIGINFO = 0x4203,
    PTRACE_GETREGSET = 0x4204,
    PTRACE_SETREGSET = 0x4205,
    PTRACE_SEIZE = 0x4206,
    PTRACE_INTERRUPT = 0x4207,
    PTRACE_LISTEN = 0x4208,
    PTRACE_PEEKSIGINFO = 0x4209,
    PTRACE_GETSIGMASK = 0x420A,
    PTRACE_SETSIGMASK = 0x420B,
    PTRACE_SECCOMP_GET_FILTER = 0x420C,
    PTRACE_SECCOMP_GET_METADATA = 0x420D,
    PTRACE_GET_SYSCALL_INFO = 0x420E,
    PTRACE_GET_RSEQ_CONFIGURATION = 0x420F,
    PTRACE_SET_SYSCALL_USER_DISPATCH_CONFIG = 0x4210,
    PTRACE_GET_SYSCALL_USER_DISPATCH_CONFIG = 0x4211,
};

// __ptrace_setoptions: 4 bytes
enum __ptrace_setoptions
{
    PTRACE_O_TRACESYSGOOD = 0x1,
    PTRACE_O_TRACEFORK = 0x2,
    PTRACE_O_TRACEVFORK = 0x4,
    PTRACE_O_TRACECLONE = 0x8,
    PTRACE_O_TRACEEXEC = 0x10,
    PTRACE_O_TRACEVFORKDONE = 0x20,
    PTRACE_O_TRACEEXIT = 0x40,
    PTRACE_O_TRACESECCOMP = 0x80,
    PTRACE_O_EXITKILL = 0x100000,
    PTRACE_O_SUSPEND_SECCOMP = 0x200000,
    PTRACE_O_MASK = 0x3000FF,
};

// __ptrace_eventcodes: 4 bytes
enum __ptrace_eventcodes
{
    PTRACE_EVENT_FORK = 1,
    PTRACE_EVENT_VFORK = 2,
    PTRACE_EVENT_CLONE = 3,
    PTRACE_EVENT_EXEC = 4,
    PTRACE_EVENT_VFORK_DONE = 5,
    PTRACE_EVENT_EXIT = 6,
    PTRACE_EVENT_SECCOMP = 7,
    PTRACE_EVENT_STOP = 128,
};

// __ptrace_get_syscall_info_op: 4 bytes
enum __ptrace_get_syscall_info_op
{
    PTRACE_SYSCALL_INFO_NONE = 0,
    PTRACE_SYSCALL_INFO_ENTRY = 1,
    PTRACE_SYSCALL_INFO_EXIT = 2,
    PTRACE_SYSCALL_INFO_SECCOMP = 3,
};

// __ptrace_peeksiginfo_flags: 4 bytes
enum __ptrace_peeksiginfo_flags
{
    PTRACE_PEEKSIGINFO_SHARED = 1,
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

typedef unsigned int size_t;

typedef int ptrdiff_t;

typedef unsigned char __u_char;

typedef unsigned short __u_short;

typedef unsigned int __u_int;

typedef unsigned long __u_long;

typedef signed char __int8_t;

typedef unsigned char __uint8_t;

typedef short __int16_t;

typedef unsigned short __uint16_t;

typedef int __int32_t;

typedef unsigned int __uint32_t;

typedef long long __int64_t;

typedef unsigned long long __uint64_t;

typedef __int8_t __int_least8_t;

typedef __uint8_t __uint_least8_t;

typedef __int16_t __int_least16_t;

typedef __uint16_t __uint_least16_t;

typedef __int32_t __int_least32_t;

typedef __uint32_t __uint_least32_t;

typedef __int64_t __int_least64_t;

typedef __uint64_t __uint_least64_t;

typedef long long __quad_t;

typedef unsigned long long __u_quad_t;

typedef long long __intmax_t;

typedef unsigned long long __uintmax_t;

typedef __uint64_t __dev_t;

typedef unsigned int __uid_t;

typedef unsigned int __gid_t;

typedef unsigned long __ino_t;

typedef __uint64_t __ino64_t;

typedef unsigned int __mode_t;

typedef unsigned int __nlink_t;

typedef long __off_t;

typedef __int64_t __off64_t;

typedef int __pid_t;

// __fsid_t: 0x8 (8) bytes
struct __fsid_t
{
    int __val[2];
};

typedef long __clock_t;

typedef unsigned long __rlim_t;

typedef __uint64_t __rlim64_t;

typedef unsigned int __id_t;

typedef long __time_t;

typedef unsigned int __useconds_t;

typedef long __suseconds_t;

typedef __int64_t __suseconds64_t;

typedef int __daddr_t;

typedef int __key_t;

typedef int __clockid_t;

typedef void *__timer_t;

typedef long __blksize_t;

typedef long __blkcnt_t;

typedef __int64_t __blkcnt64_t;

typedef unsigned long __fsblkcnt_t;

typedef __uint64_t __fsblkcnt64_t;

typedef unsigned long __fsfilcnt_t;

typedef __uint64_t __fsfilcnt64_t;

typedef int __fsword_t;

typedef int __ssize_t;

typedef long __syscall_slong_t;

typedef unsigned long __syscall_ulong_t;

typedef __off64_t __loff_t;

typedef char *__caddr_t;

typedef int __intptr_t;

typedef unsigned int __socklen_t;

typedef int __sig_atomic_t;

typedef __int64_t __time64_t;

// __mbstate_t: 0x8 (8) bytes
struct __mbstate_t
{
    int __count;
    union
    {
        unsigned int __wch;
        char __wchb[4];
    } __value;
};

// _G_fpos_t: 0xC (12) bytes
struct _G_fpos_t
{
    __off_t __pos;
    struct __mbstate_t __state;
};

typedef struct _G_fpos_t __fpos_t;

// _G_fpos64_t: 0x10 (16) bytes
struct _G_fpos64_t
{
    __off64_t __pos;
    struct __mbstate_t __state;
};

typedef struct _G_fpos64_t __fpos64_t;

typedef void _IO_lock_t;

// _IO_FILE: 0x94 (148) bytes
struct _IO_FILE
{
    int _flags;
    char *_IO_read_ptr;
    char *_IO_read_end;
    char *_IO_read_base;
    char *_IO_write_base;
    char *_IO_write_ptr;
    char *_IO_write_end;
    char *_IO_buf_base;
    char *_IO_buf_end;
    char *_IO_save_base;
    char *_IO_backup_base;
    char *_IO_save_end;
    struct _IO_marker *_markers;
    struct _IO_FILE *_chain;
    int _fileno;
    int _flags2 : 24;
    char _short_backupbuf[1];
    __off_t _old_offset;
    unsigned short _cur_column;
    signed char _vtable_offset;
    char _shortbuf[1];
    _IO_lock_t *_lock;
    __off64_t _offset;
    struct _IO_codecvt *_codecvt;
    struct _IO_wide_data *_wide_data;
    struct _IO_FILE *_freeres_list;
    void *_freeres_buf;
    struct _IO_FILE **_prevchain;
    int _mode;
    __uint64_t _total_written;
    int _unused3;
    char _unused2[28];
};

typedef struct _IO_FILE __FILE;

typedef struct _IO_FILE FILE;

// Declared only: its members are unknown.
struct _IO_marker;

// Declared only: its members are unknown.
struct _IO_codecvt;

// Declared only: its members are unknown.
struct _IO_wide_data;

typedef __ssize_t cookie_read_function_t(void *__cookie, char *__buf, size_t __nbytes);

typedef __ssize_t cookie_write_function_t(void *__cookie, const char *__buf, size_t __nbytes);

typedef int cookie_seek_function_t(void *__cookie, __off64_t *__pos, int __w);

typedef int cookie_close_function_t(void *__cookie);

// _IO_cookie_io_functions_t: 0x10 (16) bytes
struct _IO_cookie_io_functions_t
{
    cookie_read_function_t *read;
    cookie_write_function_t *write;
    cookie_seek_function_t *seek;
    cookie_close_function_t *close;
};

typedef struct _IO_cookie_io_functions_t cookie_io_functions_t;

typedef __off_t off_t;

typedef __off64_t off64_t;

typedef __ssize_t ssize_t;

typedef __fpos_t fpos_t;

typedef __fpos64_t fpos64_t;

typedef float _Float32;

typedef double _Float64;

typedef double _Float32x;

typedef long double _Float64x;

// Declared only: its members are unknown.
struct obstack;

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
    long long quot;
    long long rem;
};

// Declared only: its members are unknown.
struct __locale_data;

// __locale_struct: 0x74 (116) bytes
struct __locale_struct
{
    struct __locale_data *__locales[13];
    const unsigned short *__ctype_b;
    const int *__ctype_tolower;
    const int *__ctype_toupper;
    const char *__names[13];
};

typedef struct __locale_struct *__locale_t;

typedef __locale_t locale_t;

typedef __u_char u_char;

typedef __u_short u_short;

typedef __u_int u_int;

typedef __u_long u_long;

typedef __quad_t quad_t;

typedef __u_quad_t u_quad_t;

typedef struct __fsid_t fsid_t;

typedef __loff_t loff_t;

typedef __ino_t ino_t;

typedef __ino64_t ino64_t;

typedef __dev_t dev_t;

typedef __gid_t gid_t;

typedef __mode_t mode_t;

typedef __nlink_t nlink_t;

typedef __uid_t uid_t;

typedef __pid_t pid_t;

typedef __id_t id_t;

typedef __daddr_t daddr_t;

typedef __caddr_t caddr_t;

typedef __key_t key_t;

typedef __clock_t clock_t;

typedef __clockid_t clockid_t;

typedef __time_t time_t;

typedef __timer_t timer_t;

typedef __useconds_t useconds_t;

typedef __suseconds_t suseconds_t;

typedef unsigned long ulong;

typedef unsigned short ushort;

typedef unsigned int uint;

typedef __uint8_t u_int8_t;

typedef __uint16_t u_int16_t;

typedef __uint32_t u_int32_t;

typedef __uint64_t u_int64_t;

// __sigset_t: 0x80 (128) bytes
struct __sigset_t
{
    unsigned long __val[32];
};

typedef struct __sigset_t sigset_t;

// timeval: 0x8 (8) bytes
struct timeval
{
    __time_t tv_sec;
    __suseconds_t tv_usec;
};

// timespec: 0x8 (8) bytes
struct timespec
{
    __time_t tv_sec;
    __syscall_slong_t tv_nsec;
};

typedef long __fd_mask;

// fd_set: 0x80 (128) bytes
struct fd_set
{
    __fd_mask fds_bits[32];
};

typedef __fd_mask fd_mask;

typedef __blksize_t blksize_t;

typedef __blkcnt_t blkcnt_t;

typedef __fsblkcnt_t fsblkcnt_t;

typedef __fsfilcnt_t fsfilcnt_t;

typedef __blkcnt64_t blkcnt64_t;

typedef __fsblkcnt64_t fsblkcnt64_t;

typedef __fsfilcnt64_t fsfilcnt64_t;

// __atomic_wide_counter: 0x8 (8) bytes
union __atomic_wide_counter
{
    unsigned long long __value64;
    struct
    {
        unsigned int __low;
        unsigned int __high;
    } __value32;
};

// __pthread_internal_list: 0x8 (8) bytes
struct __pthread_internal_list
{
    struct __pthread_internal_list *__prev;
    struct __pthread_internal_list *__next;
};

typedef struct __pthread_internal_list __pthread_list_t;

// __pthread_internal_slist: 0x4 (4) bytes
struct __pthread_internal_slist
{
    struct __pthread_internal_slist *__next;
};

typedef struct __pthread_internal_slist __pthread_slist_t;

// __pthread_mutex_s: 0x18 (24) bytes
struct __pthread_mutex_s
{
    int __lock;
    unsigned int __count;
    int __owner;
    int __kind;
    unsigned int __nusers;
    union
    {
        struct
        {
            short __data_spins;
            short __data_unused;
        } __data;
        __pthread_slist_t __list;
    };
};

// __pthread_rwlock_arch_t: 0x20 (32) bytes
struct __pthread_rwlock_arch_t
{
    unsigned int __readers;
    unsigned int __writers;
    unsigned int __wrphase_futex;
    unsigned int __writers_futex;
    unsigned int __pad3;
    unsigned int __pad4;
    unsigned char __flags;
    unsigned char __shared;
    unsigned char __pad1;
    unsigned char __pad2;
    int __cur_writer;
};

// __pthread_cond_s: 0x30 (48) bytes
struct __pthread_cond_s
{
    union __atomic_wide_counter __wseq;
    union __atomic_wide_counter __g1_start;
    unsigned int __g_size[2];
    unsigned int __g1_orig_size;
    unsigned int __wrefs;
    unsigned int __g_signals[2];
    unsigned int __unused_initialized_1;
    unsigned int __unused_initialized_2;
};

typedef unsigned int __tss_t;

typedef unsigned long __thrd_t;

// __once_flag: 0x4 (4) bytes
struct __once_flag
{
    int __data;
};

typedef unsigned long pthread_t;

// pthread_mutexattr_t: 0x4 (4) bytes
union pthread_mutexattr_t
{
    char __size[4];
    int __align;
};

// pthread_condattr_t: 0x4 (4) bytes
union pthread_condattr_t
{
    char __size[4];
    int __align;
};

typedef unsigned int pthread_key_t;

typedef int pthread_once_t;

// pthread_attr_t: 0x24 (36) bytes
union pthread_attr_t
{
    char __size[36];
    long __align;
};

// pthread_mutex_t: 0x18 (24) bytes
union pthread_mutex_t
{
    struct __pthread_mutex_s __data;
    char __size[24];
    long __align;
};

// pthread_cond_t: 0x30 (48) bytes
union pthread_cond_t
{
    struct __pthread_cond_s __data;
    char __size[48];
    long long __align;
};

// pthread_rwlock_t: 0x20 (32) bytes
union pthread_rwlock_t
{
    struct __pthread_rwlock_arch_t __data;
    char __size[32];
    long __align;
};

// pthread_rwlockattr_t: 0x8 (8) bytes
union pthread_rwlockattr_t
{
    char __size[8];
    long __align;
};

typedef volatile int pthread_spinlock_t;

// pthread_barrier_t: 0x14 (20) bytes
union pthread_barrier_t
{
    char __size[20];
    long __align;
};

// pthread_barrierattr_t: 0x4 (4) bytes
union pthread_barrierattr_t
{
    char __size[4];
    int __align;
};

// random_data: 0x1C (28) bytes
struct random_data
{
    int *fptr;
    int *rptr;
    int *state;
    int rand_type;
    int rand_deg;
    int rand_sep;
    int *end_ptr;
};

// drand48_data: 0x18 (24) bytes
struct drand48_data
{
    unsigned short __x[3];
    unsigned short __old_x[3];
    unsigned short __c;
    unsigned short __init;
    unsigned long long __a;
};

typedef int (*__compar_fn_t)(const void *, const void *);

typedef __compar_fn_t comparison_fn_t;

typedef int (*__compar_d_fn_t)(const void *, const void *, void *);

typedef struct __once_flag once_flag;

typedef __intptr_t intptr_t;

typedef __socklen_t socklen_t;

// flock: 0x10 (16) bytes
struct flock
{
    short l_type;
    short l_whence;
    __off_t l_start;
    __off_t l_len;
    __pid_t l_pid;
};

// flock64: 0x18 (24) bytes
struct flock64
{
    short l_type;
    short l_whence;
    __off64_t l_start;
    __off64_t l_len;
    __pid_t l_pid;
};

// iovec: 0x8 (8) bytes
struct iovec
{
    void *iov_base;
    size_t iov_len;
};

// f_owner_ex: 0x8 (8) bytes
struct f_owner_ex
{
    enum __pid_type type;
    __pid_t pid;
};

// file_handle: 0x8 (8) bytes
struct file_handle
{
    unsigned int handle_bytes;
    int handle_type;
    unsigned char f_handle[0];
};

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

typedef unsigned long long __kernel_uoff_t;

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

// open_how: 0x18 (24) bytes
struct open_how
{
    __u64 flags;
    __u64 mode;
    __u64 resolve;
};

// stat: 0x58 (88) bytes
struct stat
{
    __dev_t st_dev;
    unsigned short __pad1;
    __ino_t st_ino;
    __mode_t st_mode;
    __nlink_t st_nlink;
    __uid_t st_uid;
    __gid_t st_gid;
    __dev_t st_rdev;
    unsigned short __pad2;
    __off_t st_size;
    __blksize_t st_blksize;
    __blkcnt_t st_blocks;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
    unsigned long __glibc_reserved4;
    unsigned long __glibc_reserved5;
};

// stat64: 0x60 (96) bytes
struct stat64
{
    __dev_t st_dev;
    unsigned int __pad1;
    __ino_t __st_ino;
    __mode_t st_mode;
    __nlink_t st_nlink;
    __uid_t st_uid;
    __gid_t st_gid;
    __dev_t st_rdev;
    unsigned int __pad2;
    __off64_t st_size;
    __blksize_t st_blksize;
    __blkcnt64_t st_blocks;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
    __ino64_t st_ino;
};

typedef int error_t;

typedef unsigned int wint_t;

typedef struct __mbstate_t mbstate_t;

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

typedef unsigned long wctype_t;

typedef const __int32_t *wctrans_t;

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

// timex: 0x80 (128) bytes
struct timex
{
    unsigned int modes;
    __syscall_slong_t offset;
    __syscall_slong_t freq;
    __syscall_slong_t maxerror;
    __syscall_slong_t esterror;
    int status;
    __syscall_slong_t constant;
    __syscall_slong_t precision;
    __syscall_slong_t tolerance;
    struct timeval time;
    __syscall_slong_t tick;
    __syscall_slong_t ppsfreq;
    __syscall_slong_t jitter;
    int shift;
    __syscall_slong_t stabil;
    __syscall_slong_t jitcnt;
    __syscall_slong_t calcnt;
    __syscall_slong_t errcnt;
    __syscall_slong_t stbcnt;
    int tai;
    unsigned char __filler0[44];
};

// itimerspec: 0x10 (16) bytes
struct itimerspec
{
    struct timespec it_interval;
    struct timespec it_value;
};

// sigval: 0x4 (4) bytes
union sigval
{
    int sival_int;
    void *sival_ptr;
};

typedef union sigval __sigval_t;

// sigevent: 0x40 (64) bytes
struct sigevent
{
    __sigval_t sigev_value;
    int sigev_signo;
    int sigev_notify;
    union
    {
        int _pad[13];
        __pid_t _tid;
        struct
        {
            void (*_function)(__sigval_t);
            union pthread_attr_t *_attribute;
        } _sigev_thread;
    } _sigev_un;
};

typedef __sig_atomic_t sig_atomic_t;

// siginfo_t: 0x80 (128) bytes
struct siginfo_t
{
    int si_signo;
    int si_errno;
    int si_code;
    union
    {
        int _pad[29];
        struct
        {
            __pid_t si_pid;
            __uid_t si_uid;
        } _kill;
        struct
        {
            int si_tid;
            int si_overrun;
            __sigval_t si_sigval;
        } _timer;
        struct
        {
            __pid_t si_pid;
            __uid_t si_uid;
            __sigval_t si_sigval;
        } _rt;
        struct
        {
            __pid_t si_pid;
            __uid_t si_uid;
            int si_status;
            __clock_t si_utime;
            __clock_t si_stime;
        } _sigchld;
        struct
        {
            void *si_addr;
            short si_addr_lsb;
            union
            {
                struct
                {
                    void *_lower;
                    void *_upper;
                } _addr_bnd;
                __uint32_t _pkey;
            } _bounds;
        } _sigfault;
        struct
        {
            long si_band;
            int si_fd;
        } _sigpoll;
        struct
        {
            void *_call_addr;
            int _syscall;
            unsigned int _arch;
        } _sigsys;
    } _sifields;
};

typedef __sigval_t sigval_t;

typedef struct sigevent sigevent_t;

typedef void (*__sighandler_t)(int);

typedef __sighandler_t sighandler_t;

typedef __sighandler_t sig_t;

// sigaction: 0x8C (140) bytes
struct sigaction
{
    union
    {
        __sighandler_t sa_handler;
        void (*sa_sigaction)(int, struct siginfo_t *, void *);
    } __sigaction_handler;
    struct __sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

// _fpx_sw_bytes: 0x30 (48) bytes
struct _fpx_sw_bytes
{
    __uint32_t magic1;
    __uint32_t extended_size;
    __uint64_t xstate_bv;
    __uint32_t xstate_size;
    __uint32_t __glibc_reserved1[7];
};

// _fpreg: 0xA (10) bytes
struct _fpreg
{
    unsigned short significand[4];
    unsigned short exponent;
};

// _fpxreg: 0x10 (16) bytes
struct _fpxreg
{
    unsigned short significand[4];
    unsigned short exponent;
    unsigned short __glibc_reserved1[3];
};

// _xmmreg: 0x10 (16) bytes
struct _xmmreg
{
    __uint32_t element[4];
};

// _fpstate: 0x270 (624) bytes
struct _fpstate
{
    __uint32_t cw;
    __uint32_t sw;
    __uint32_t tag;
    __uint32_t ipoff;
    __uint32_t cssel;
    __uint32_t dataoff;
    __uint32_t datasel;
    struct _fpreg _st[8];
    unsigned short status;
    unsigned short magic;
    __uint32_t _fxsr_env[6];
    __uint32_t mxcsr;
    __uint32_t __glibc_reserved1;
    struct _fpxreg _fxsr_st[8];
    struct _xmmreg _xmm[8];
    __uint32_t __glibc_reserved2[56];
};

// sigcontext: 0x58 (88) bytes
struct sigcontext
{
    unsigned short gs;
    unsigned short __gsh;
    unsigned short fs;
    unsigned short __fsh;
    unsigned short es;
    unsigned short __esh;
    unsigned short ds;
    unsigned short __dsh;
    unsigned long edi;
    unsigned long esi;
    unsigned long ebp;
    unsigned long esp;
    unsigned long ebx;
    unsigned long edx;
    unsigned long ecx;
    unsigned long eax;
    unsigned long trapno;
    unsigned long err;
    unsigned long eip;
    unsigned short cs;
    unsigned short __csh;
    unsigned long eflags;
    unsigned long esp_at_signal;
    unsigned short ss;
    unsigned short __ssh;
    struct _fpstate *fpstate;
    unsigned long oldmask;
    unsigned long cr2;
};

// _xsave_hdr: 0x40 (64) bytes
struct _xsave_hdr
{
    __uint64_t xstate_bv;
    __uint64_t __glibc_reserved1[2];
    __uint64_t __glibc_reserved2[5];
};

// _ymmh_state: 0x100 (256) bytes
struct _ymmh_state
{
    __uint32_t ymmh_space[64];
};

// _xstate: 0x3B0 (944) bytes
struct _xstate
{
    struct _fpstate fpstate;
    struct _xsave_hdr xstate_hdr;
    struct _ymmh_state ymmh;
};

// stack_t: 0xC (12) bytes
struct stack_t
{
    void *ss_sp;
    int ss_flags;
    size_t ss_size;
};

typedef int greg_t;

typedef greg_t gregset_t[19];

// _libc_fpreg: 0xA (10) bytes
struct _libc_fpreg
{
    unsigned short significand[4];
    unsigned short exponent;
};

// _libc_fpstate: 0x70 (112) bytes
struct _libc_fpstate
{
    unsigned long cw;
    unsigned long sw;
    unsigned long tag;
    unsigned long ipoff;
    unsigned long cssel;
    unsigned long dataoff;
    unsigned long datasel;
    struct _libc_fpreg _st[8];
    unsigned long status;
};

typedef struct _libc_fpstate *fpregset_t;

// mcontext_t: 0x58 (88) bytes
struct mcontext_t
{
    gregset_t gregs;
    fpregset_t fpregs;
    unsigned long oldmask;
    unsigned long cr2;
};

// ucontext_t: 0x16C (364) bytes
struct ucontext_t
{
    unsigned long uc_flags;
    struct ucontext_t *uc_link;
    struct stack_t uc_stack;
    struct mcontext_t uc_mcontext;
    sigset_t uc_sigmask;
    struct _libc_fpstate __fpregs_mem;
    unsigned long __ssp[4];
};

// sigstack: 0x8 (8) bytes
struct sigstack
{
    void *ss_sp;
    int ss_onstack;
};

typedef int __jmp_buf[6];

// __jmp_buf_tag: 0x9C (156) bytes
struct __jmp_buf_tag
{
    __jmp_buf __jmpbuf;
    int __mask_was_saved;
    struct __sigset_t __saved_mask;
};

typedef struct __jmp_buf_tag jmp_buf[1];

typedef struct __jmp_buf_tag sigjmp_buf[1];

typedef long double float_t;

typedef long double double_t;

typedef long double long_double_t;

typedef long double _Float32_t;

typedef long double _Float64_t;

typedef unsigned short fexcept_t;

// fenv_t: 0x1C (28) bytes
struct fenv_t
{
    unsigned short __control_word;
    unsigned short __glibc_reserved1;
    unsigned short __status_word;
    unsigned short __glibc_reserved2;
    unsigned short __tags;
    unsigned short __glibc_reserved3;
    unsigned int __eip;
    unsigned short __cs_selector;
    unsigned int __opcode : 11;
    unsigned int __glibc_reserved4 : 5;
    unsigned int __data_offset;
    unsigned short __data_selector;
    unsigned short __glibc_reserved5;
};

// femode_t: 0x8 (8) bytes
struct femode_t
{
    unsigned short __control_word;
    unsigned short __glibc_reserved;
    unsigned int __mxcsr;
};

typedef __int_least8_t int_least8_t;

typedef __int_least16_t int_least16_t;

typedef __int_least32_t int_least32_t;

typedef __int_least64_t int_least64_t;

typedef __uint_least8_t uint_least8_t;

typedef __uint_least16_t uint_least16_t;

typedef __uint_least32_t uint_least32_t;

typedef __uint_least64_t uint_least64_t;

typedef signed char int_fast8_t;

typedef int int_fast16_t;

typedef int int_fast32_t;

typedef long long int_fast64_t;

typedef unsigned char uint_fast8_t;

typedef unsigned int uint_fast16_t;

typedef unsigned int uint_fast32_t;

typedef unsigned long long uint_fast64_t;

typedef unsigned int uintptr_t;

typedef __intmax_t intmax_t;

typedef __uintmax_t uintmax_t;

typedef int __gwchar_t;

// imaxdiv_t: 0x10 (16) bytes
struct imaxdiv_t
{
    long long quot;
    long long rem;
};

// dirent: 0x10C (268) bytes
struct dirent
{
    __ino_t d_ino;
    __off_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[256];
};

// dirent64: 0x114 (276) bytes
struct dirent64
{
    __ino64_t d_ino;
    __off64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[256];
};

// Declared only: its members are unknown.
struct __dirstream;

typedef struct __dirstream DIR;

typedef long Lmid_t;

// Dl_info: 0x10 (16) bytes
struct Dl_info
{
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
};

// Dl_serpath: 0x8 (8) bytes
struct Dl_serpath
{
    char *dls_name;
    unsigned int dls_flags;
};

// Dl_serinfo: 0x10 (16) bytes
struct Dl_serinfo
{
    size_t dls_size;
    unsigned int dls_cnt;
    union
    {
        struct Dl_serpath dls_serpath[0];
        struct Dl_serpath __dls_serpath_pad[1];
    };
};

typedef unsigned int Elf32_Addr;

// link_map: 0x14 (20) bytes
struct link_map
{
    Elf32_Addr l_addr;
    char *l_name;
    struct Elf32_Dyn *l_ld;
    struct link_map *l_next;
    struct link_map *l_prev;
};

// dl_find_object: 0x58 (88) bytes
struct dl_find_object
{
    unsigned long long dlfo_flags;
    void *dlfo_map_start;
    void *dlfo_map_end;
    struct link_map *dlfo_link_map;
    void *dlfo_eh_frame;
    void *dlfo_eh_dbase;
    unsigned int __dlfo_eh_dbase_pad;
    void *dlfo_sframe;
    unsigned int __dlfo_sframe_pad;
    unsigned long long __dlfo_reserved[6];
};

// sched_attr: 0x38 (56) bytes
struct sched_attr
{
    __u32 size;
    __u32 sched_policy;
    __u64 sched_flags;
    __s32 sched_nice;
    __u32 sched_priority;
    __u64 sched_runtime;
    __u64 sched_deadline;
    __u64 sched_period;
    __u32 sched_util_min;
    __u32 sched_util_max;
};

// sched_param: 0x4 (4) bytes
struct sched_param
{
    int sched_priority;
};

typedef unsigned long __cpu_mask;

// cpu_set_t: 0x80 (128) bytes
struct cpu_set_t
{
    __cpu_mask __bits[32];
};

// _pthread_cleanup_buffer: 0x10 (16) bytes
struct _pthread_cleanup_buffer
{
    void (*__routine)(void *);
    void *__arg;
    int __canceltype;
    struct _pthread_cleanup_buffer *__prev;
};

// __cancel_jmp_buf_tag: 0x1C (28) bytes
struct __cancel_jmp_buf_tag
{
    __jmp_buf __cancel_jmp_buf;
    int __mask_was_saved;
};

// __pthread_cleanup_frame: 0x10 (16) bytes
struct __pthread_cleanup_frame
{
    void (*__cancel_routine)(void *);
    void *__cancel_arg;
    int __do_it;
    int __cancel_type;
};

// sem_t: 0x10 (16) bytes
union sem_t
{
    char __size[16];
    long __align;
};

typedef unsigned long nfds_t;

// pollfd: 0x8 (8) bytes
struct pollfd
{
    int fd;
    short events;
    short revents;
};

typedef unsigned char cc_t;

typedef unsigned int speed_t;

typedef unsigned int tcflag_t;

// termios: 0x3C (60) bytes
struct termios
{
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_line;
    cc_t c_cc[32];
    union
    {
        speed_t __ispeed;
        speed_t c_ispeed;
    };
    union
    {
        speed_t __ospeed;
        speed_t c_ospeed;
    };
};

typedef speed_t baud_t;

// passwd: 0x1C (28) bytes
struct passwd
{
    char *pw_name;
    char *pw_passwd;
    __uid_t pw_uid;
    __gid_t pw_gid;
    char *pw_gecos;
    char *pw_dir;
    char *pw_shell;
};

// group: 0x10 (16) bytes
struct group
{
    char *gr_name;
    char *gr_passwd;
    __gid_t gr_gid;
    char **gr_mem;
};

typedef unsigned int __size_t;

// glob_t: 0x24 (36) bytes
struct glob_t
{
    __size_t gl_pathc;
    char **gl_pathv;
    __size_t gl_offs;
    int gl_flags;
    void (*gl_closedir)(void *);
    struct dirent *(*gl_readdir)(void *);
    void *(*gl_opendir)(const char *);
    int (*gl_lstat)(const char *, struct stat *);
    int (*gl_stat)(const char *, struct stat *);
};

// glob64_t: 0x24 (36) bytes
struct glob64_t
{
    __size_t gl_pathc;
    char **gl_pathv;
    __size_t gl_offs;
    int gl_flags;
    void (*gl_closedir)(void *);
    struct dirent64 *(*gl_readdir)(void *);
    void *(*gl_opendir)(const char *);
    int (*gl_lstat)(const char *, struct stat64 *);
    int (*gl_stat)(const char *, struct stat64 *);
};

typedef unsigned int __re_size_t;

typedef unsigned long __re_long_size_t;

typedef long s_reg_t;

typedef unsigned long active_reg_t;

typedef unsigned long reg_syntax_t;

// Declared only: its members are unknown.
struct re_dfa_t;

// re_pattern_buffer: 0x20 (32) bytes
struct re_pattern_buffer
{
    struct re_dfa_t *buffer;
    __re_long_size_t allocated;
    __re_long_size_t used;
    reg_syntax_t syntax;
    char *fastmap;
    unsigned char *translate;
    size_t re_nsub;
    unsigned int can_be_null : 1;
    unsigned int regs_allocated : 2;
    unsigned int fastmap_accurate : 1;
    unsigned int no_sub : 1;
    unsigned int not_bol : 1;
    unsigned int not_eol : 1;
    unsigned int newline_anchor : 1;
};

typedef struct re_pattern_buffer regex_t;

typedef int regoff_t;

// re_registers: 0xC (12) bytes
struct re_registers
{
    __re_size_t num_regs;
    regoff_t *start;
    regoff_t *end;
};

// regmatch_t: 0x8 (8) bytes
struct regmatch_t
{
    regoff_t rm_so;
    regoff_t rm_eo;
};

// option: 0x10 (16) bytes
struct option
{
    const char *name;
    int has_arg;
    int *flag;
    int val;
};

// posix_spawnattr_t: 0x150 (336) bytes
struct posix_spawnattr_t
{
    short __flags;
    pid_t __pgrp;
    sigset_t __sd;
    sigset_t __ss;
    struct sched_param __sp;
    int __policy;
    int __cgroup;
    int __pad[15];
};

// Declared only: its members are unknown.
struct __spawn_action;

// posix_spawn_file_actions_t: 0x4C (76) bytes
struct posix_spawn_file_actions_t
{
    int __allocated;
    int __used;
    struct __spawn_action *__actions;
    int __pad[16];
};

typedef void *iconv_t;

typedef void *nl_catd;

typedef int nl_item;

typedef unsigned short Elf32_Half;

typedef unsigned short Elf64_Half;

typedef unsigned int Elf32_Word;

typedef int Elf32_Sword;

typedef unsigned int Elf64_Word;

typedef int Elf64_Sword;

typedef unsigned long long Elf32_Xword;

typedef long long Elf32_Sxword;

typedef unsigned long long Elf64_Xword;

typedef long long Elf64_Sxword;

typedef unsigned long long Elf64_Addr;

typedef unsigned int Elf32_Off;

typedef unsigned long long Elf64_Off;

typedef unsigned short Elf32_Section;

typedef unsigned short Elf64_Section;

typedef Elf32_Half Elf32_Versym;

typedef Elf64_Half Elf64_Versym;

// Elf32_Ehdr: 0x34 (52) bytes
struct Elf32_Ehdr
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

// Elf64_Ehdr: 0x40 (64) bytes
struct Elf64_Ehdr
{
    unsigned char e_ident[16];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off e_phoff;
    Elf64_Off e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
};

// Elf32_Shdr: 0x28 (40) bytes
struct Elf32_Shdr
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

// Elf64_Shdr: 0x40 (64) bytes
struct Elf64_Shdr
{
    Elf64_Word sh_name;
    Elf64_Word sh_type;
    Elf64_Xword sh_flags;
    Elf64_Addr sh_addr;
    Elf64_Off sh_offset;
    Elf64_Xword sh_size;
    Elf64_Word sh_link;
    Elf64_Word sh_info;
    Elf64_Xword sh_addralign;
    Elf64_Xword sh_entsize;
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
    Elf64_Xword ch_size;
    Elf64_Xword ch_addralign;
};

// Elf32_Sym: 0x10 (16) bytes
struct Elf32_Sym
{
    Elf32_Word st_name;
    Elf32_Addr st_value;
    Elf32_Word st_size;
    unsigned char st_info;
    unsigned char st_other;
    Elf32_Section st_shndx;
};

// Elf64_Sym: 0x18 (24) bytes
struct Elf64_Sym
{
    Elf64_Word st_name;
    unsigned char st_info;
    unsigned char st_other;
    Elf64_Section st_shndx;
    Elf64_Addr st_value;
    Elf64_Xword st_size;
};

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

// Elf32_Rel: 0x8 (8) bytes
struct Elf32_Rel
{
    Elf32_Addr r_offset;
    Elf32_Word r_info;
};

// Elf64_Rel: 0x10 (16) bytes
struct Elf64_Rel
{
    Elf64_Addr r_offset;
    Elf64_Xword r_info;
};

// Elf32_Rela: 0xC (12) bytes
struct Elf32_Rela
{
    Elf32_Addr r_offset;
    Elf32_Word r_info;
    Elf32_Sword r_addend;
};

// Elf64_Rela: 0x18 (24) bytes
struct Elf64_Rela
{
    Elf64_Addr r_offset;
    Elf64_Xword r_info;
    Elf64_Sxword r_addend;
};

typedef Elf32_Word Elf32_Relr;

typedef Elf64_Xword Elf64_Relr;

// Elf32_Phdr: 0x20 (32) bytes
struct Elf32_Phdr
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

// Elf64_Phdr: 0x38 (56) bytes
struct Elf64_Phdr
{
    Elf64_Word p_type;
    Elf64_Word p_flags;
    Elf64_Off p_offset;
    Elf64_Addr p_vaddr;
    Elf64_Addr p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
};

// Elf32_Dyn: 0x8 (8) bytes
struct Elf32_Dyn
{
    Elf32_Sword d_tag;
    union
    {
        Elf32_Word d_val;
        Elf32_Addr d_ptr;
    } d_un;
};

// Elf64_Dyn: 0x10 (16) bytes
struct Elf64_Dyn
{
    Elf64_Sxword d_tag;
    union
    {
        Elf64_Xword d_val;
        Elf64_Addr d_ptr;
    } d_un;
};

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

// Elf32_Verdaux: 0x8 (8) bytes
struct Elf32_Verdaux
{
    Elf32_Word vda_name;
    Elf32_Word vda_next;
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

// Elf64_Verneed: 0x10 (16) bytes
struct Elf64_Verneed
{
    Elf64_Half vn_version;
    Elf64_Half vn_cnt;
    Elf64_Word vn_file;
    Elf64_Word vn_aux;
    Elf64_Word vn_next;
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

// Elf64_Vernaux: 0x10 (16) bytes
struct Elf64_Vernaux
{
    Elf64_Word vna_hash;
    Elf64_Half vna_flags;
    Elf64_Half vna_other;
    Elf64_Word vna_name;
    Elf64_Word vna_next;
};

// Elf32_auxv_t: 0x8 (8) bytes
struct Elf32_auxv_t
{
    unsigned int a_type;
    union
    {
        unsigned int a_val;
    } a_un;
};

// Elf64_auxv_t: 0x10 (16) bytes
struct Elf64_auxv_t
{
    unsigned long long a_type;
    union
    {
        unsigned long long a_val;
    } a_un;
};

// Elf32_Nhdr: 0xC (12) bytes
struct Elf32_Nhdr
{
    Elf32_Word n_namesz;
    Elf32_Word n_descsz;
    Elf32_Word n_type;
};

// Elf64_Nhdr: 0xC (12) bytes
struct Elf64_Nhdr
{
    Elf64_Word n_namesz;
    Elf64_Word n_descsz;
    Elf64_Word n_type;
};

// Elf32_Move: 0x14 (20) bytes
struct Elf32_Move
{
    Elf32_Xword m_value;
    Elf32_Word m_info;
    Elf32_Word m_poffset;
    Elf32_Half m_repeat;
    Elf32_Half m_stride;
};

// Elf64_Move: 0x1C (28) bytes
struct Elf64_Move
{
    Elf64_Xword m_value;
    Elf64_Xword m_info;
    Elf64_Xword m_poffset;
    Elf64_Half m_repeat;
    Elf64_Half m_stride;
};

// Elf32_gptab: 0x8 (8) bytes
union Elf32_gptab
{
    struct
    {
        Elf32_Word gt_current_g_value;
        Elf32_Word gt_unused;
    } gt_header;
    struct
    {
        Elf32_Word gt_g_value;
        Elf32_Word gt_bytes;
    } gt_entry;
};

// Elf32_RegInfo: 0x18 (24) bytes
struct Elf32_RegInfo
{
    Elf32_Word ri_gprmask;
    Elf32_Word ri_cprmask[4];
    Elf32_Sword ri_gp_value;
};

// Elf_Options: 0x8 (8) bytes
struct Elf_Options
{
    unsigned char kind;
    unsigned char size;
    Elf32_Section section;
    Elf32_Word info;
};

// Elf_Options_Hw: 0x8 (8) bytes
struct Elf_Options_Hw
{
    Elf32_Word hwp_flags1;
    Elf32_Word hwp_flags2;
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

typedef Elf32_Addr Elf32_Conflict;

// Elf_MIPS_ABIFlags_v0: 0x18 (24) bytes
struct Elf_MIPS_ABIFlags_v0
{
    Elf32_Half version;
    unsigned char isa_level;
    unsigned char isa_rev;
    unsigned char gpr_size;
    unsigned char cpr1_size;
    unsigned char cpr2_size;
    unsigned char fp_abi;
    Elf32_Word isa_ext;
    Elf32_Word ases;
    Elf32_Word flags1;
    Elf32_Word flags2;
};

typedef unsigned int Elf_Symndx;

// La_i86_regs: 0x14 (20) bytes
struct La_i86_regs
{
    unsigned int lr_edx;
    unsigned int lr_ecx;
    unsigned int lr_eax;
    unsigned int lr_ebp;
    unsigned int lr_esp;
};

// La_i86_retval: 0x30 (48) bytes
struct La_i86_retval
{
    unsigned int lrv_eax;
    unsigned int lrv_edx;
    long double lrv_st0;
    long double lrv_st1;
    unsigned long long __glibc_unused1;
    unsigned long long __glibc_unused2;
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

// r_debug_extended: 0x18 (24) bytes
struct r_debug_extended
{
    struct r_debug base;
    struct r_debug_extended *r_next;
};

// dl_phdr_info: 0x28 (40) bytes
struct dl_phdr_info
{
    Elf32_Addr dlpi_addr;
    const char *dlpi_name;
    const struct Elf32_Phdr *dlpi_phdr;
    Elf32_Half dlpi_phnum;
    unsigned long long dlpi_adds;
    unsigned long long dlpi_subs;
    size_t dlpi_tls_modid;
    void *dlpi_tls_data;
};

typedef unsigned short sa_family_t;

// sockaddr: 0x10 (16) bytes
struct sockaddr
{
    sa_family_t sa_family;
    char sa_data[14];
};

// sockaddr_storage: 0x80 (128) bytes
struct sockaddr_storage
{
    sa_family_t ss_family;
    char __ss_padding[122];
    unsigned long __ss_align;
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

// cmsghdr: 0xC (12) bytes
struct cmsghdr
{
    size_t cmsg_len;
    int cmsg_level;
    int cmsg_type;
    unsigned char __cmsg_data[];
};

// ucred: 0xC (12) bytes
struct ucred
{
    pid_t pid;
    uid_t uid;
    gid_t gid;
};

// linger: 0x8 (8) bytes
struct linger
{
    int l_onoff;
    int l_linger;
};

// osockaddr: 0x10 (16) bytes
struct osockaddr
{
    unsigned short sa_family;
    unsigned char sa_data[14];
};

// Declared only: its members are unknown.
struct sockaddr_at;

// Declared only: its members are unknown.
struct sockaddr_ax25;

// Declared only: its members are unknown.
struct sockaddr_dl;

// Declared only: its members are unknown.
struct sockaddr_eon;

typedef unsigned short in_port_t;

typedef unsigned int in_addr_t;

// in_addr: 0x4 (4) bytes
struct in_addr
{
    in_addr_t s_addr;
};

// sockaddr_in: 0x10 (16) bytes
struct sockaddr_in
{
    sa_family_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    unsigned char sin_zero[8];
};

// in6_addr: 0x10 (16) bytes
struct in6_addr
{
    union
    {
        unsigned char __u6_addr8[16];
        unsigned short __u6_addr16[8];
        unsigned int __u6_addr32[4];
    } __in6_u;
};

// sockaddr_in6: 0x1C (28) bytes
struct sockaddr_in6
{
    sa_family_t sin6_family;
    in_port_t sin6_port;
    unsigned int sin6_flowinfo;
    struct in6_addr sin6_addr;
    unsigned int sin6_scope_id;
};

// Declared only: its members are unknown.
struct sockaddr_inarp;

// Declared only: its members are unknown.
struct sockaddr_ipx;

// Declared only: its members are unknown.
struct sockaddr_iso;

// Declared only: its members are unknown.
struct sockaddr_ns;

// sockaddr_un: 0x6E (110) bytes
struct sockaddr_un
{
    sa_family_t sun_family;
    char sun_path[108];
};

// Declared only: its members are unknown.
struct sockaddr_x25;

// __SOCKADDR_ARG: 0x4 (4) bytes
union __SOCKADDR_ARG
{
    struct sockaddr *__sockaddr__;
    struct sockaddr_at *__sockaddr_at__;
    struct sockaddr_ax25 *__sockaddr_ax25__;
    struct sockaddr_dl *__sockaddr_dl__;
    struct sockaddr_eon *__sockaddr_eon__;
    struct sockaddr_in *__sockaddr_in__;
    struct sockaddr_in6 *__sockaddr_in6__;
    struct sockaddr_inarp *__sockaddr_inarp__;
    struct sockaddr_ipx *__sockaddr_ipx__;
    struct sockaddr_iso *__sockaddr_iso__;
    struct sockaddr_ns *__sockaddr_ns__;
    struct sockaddr_un *__sockaddr_un__;
    struct sockaddr_x25 *__sockaddr_x25__;
};

// __CONST_SOCKADDR_ARG: 0x4 (4) bytes
union __CONST_SOCKADDR_ARG
{
    const struct sockaddr *__sockaddr__;
    const struct sockaddr_at *__sockaddr_at__;
    const struct sockaddr_ax25 *__sockaddr_ax25__;
    const struct sockaddr_dl *__sockaddr_dl__;
    const struct sockaddr_eon *__sockaddr_eon__;
    const struct sockaddr_in *__sockaddr_in__;
    const struct sockaddr_in6 *__sockaddr_in6__;
    const struct sockaddr_inarp *__sockaddr_inarp__;
    const struct sockaddr_ipx *__sockaddr_ipx__;
    const struct sockaddr_iso *__sockaddr_iso__;
    const struct sockaddr_ns *__sockaddr_ns__;
    const struct sockaddr_un *__sockaddr_un__;
    const struct sockaddr_x25 *__sockaddr_x25__;
};

// mmsghdr: 0x20 (32) bytes
struct mmsghdr
{
    struct msghdr msg_hdr;
    unsigned int msg_len;
};

// ip_opts: 0x2C (44) bytes
struct ip_opts
{
    struct in_addr ip_dst;
    char ip_opts[40];
};

// in_pktinfo: 0xC (12) bytes
struct in_pktinfo
{
    int ipi_ifindex;
    struct in_addr ipi_spec_dst;
    struct in_addr ipi_addr;
};

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

// ip_mreq_source: 0xC (12) bytes
struct ip_mreq_source
{
    struct in_addr imr_multiaddr;
    struct in_addr imr_interface;
    struct in_addr imr_sourceaddr;
};

// ipv6_mreq: 0x14 (20) bytes
struct ipv6_mreq
{
    struct in6_addr ipv6mr_multiaddr;
    unsigned int ipv6mr_interface;
};

// group_req: 0x84 (132) bytes
struct group_req
{
    unsigned int gr_interface;
    struct sockaddr_storage gr_group;
};

// group_source_req: 0x104 (260) bytes
struct group_source_req
{
    unsigned int gsr_interface;
    struct sockaddr_storage gsr_group;
    struct sockaddr_storage gsr_source;
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

// group_filter: 0x10C (268) bytes
struct group_filter
{
    unsigned int gf_interface;
    struct sockaddr_storage gf_group;
    unsigned int gf_fmode;
    unsigned int gf_numsrc;
    struct sockaddr_storage gf_slist[1];
};

// in6_pktinfo: 0x14 (20) bytes
struct in6_pktinfo
{
    struct in6_addr ipi6_addr;
    unsigned int ipi6_ifindex;
};

// ip6_mtuinfo: 0x20 (32) bytes
struct ip6_mtuinfo
{
    struct sockaddr_in6 ip6m_addr;
    unsigned int ip6m_mtu;
};

// rpcent: 0xC (12) bytes
struct rpcent
{
    char *r_name;
    char **r_aliases;
    int r_number;
};

// netent: 0x10 (16) bytes
struct netent
{
    char *n_name;
    char **n_aliases;
    int n_addrtype;
    unsigned int n_net;
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
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct addrinfo *ai_next;
};

// gaicb: 0x28 (40) bytes
struct gaicb
{
    const char *ar_name;
    const char *ar_service;
    const struct addrinfo *ar_request;
    struct addrinfo *ar_result;
    int __return;
    int __glibc_reserved[5];
};

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

// qelem: 0xC (12) bytes
struct qelem
{
    struct qelem *q_forw;
    struct qelem *q_back;
    char q_data[1];
};

// entry: 0x8 (8) bytes
struct entry
{
    char *key;
    void *data;
};

typedef struct entry ENTRY;

// Declared only: its members are unknown.
struct _ENTRY;

// hsearch_data: 0xC (12) bytes
struct hsearch_data
{
    struct _ENTRY *table;
    unsigned int size;
    unsigned int filled;
};

typedef void (*__action_fn_t)(const void *__nodep, enum VISIT __value, int __level);

typedef void (*__free_fn_t)(void *__nodep);

// utimbuf: 0x8 (8) bytes
struct utimbuf
{
    __time_t actime;
    __time_t modtime;
};

// timezone: 0x8 (8) bytes
struct timezone
{
    int tz_minuteswest;
    int tz_dsttime;
};

// itimerval: 0x10 (16) bytes
struct itimerval
{
    struct timeval it_interval;
    struct timeval it_value;
};

typedef enum __itimer_which __itimer_which_t;

// lastlog: 0x124 (292) bytes
struct lastlog
{
    __uint32_t ll_time;
    char ll_line[32];
    char ll_host[256];
};

// exit_status: 0x4 (4) bytes
struct exit_status
{
    short e_termination;
    short e_exit;
};

// utmp: 0x180 (384) bytes
struct utmp
{
    short ut_type;
    pid_t ut_pid;
    char ut_line[32];
    char ut_id[4];
    char ut_user[32];
    char ut_host[256];
    struct exit_status ut_exit;
    int ut_session;
    struct
    {
        __uint32_t tv_sec;
        int tv_usec;
    } ut_tv;
    int ut_addr_v6[4];
    char __glibc_reserved[20];
};

// statx_timestamp: 0x10 (16) bytes
struct statx_timestamp
{
    __s64 tv_sec;
    __u32 tv_nsec;
    __s32 __reserved;
};

// statx: 0x100 (256) bytes
struct statx
{
    __u32 stx_mask;
    __u32 stx_blksize;
    __u64 stx_attributes;
    __u32 stx_nlink;
    __u32 stx_uid;
    __u32 stx_gid;
    __u16 stx_mode;
    __u16 __spare0[1];
    __u64 stx_ino;
    __u64 stx_size;
    __u64 stx_blocks;
    __u64 stx_attributes_mask;
    struct statx_timestamp stx_atime;
    struct statx_timestamp stx_btime;
    struct statx_timestamp stx_ctime;
    struct statx_timestamp stx_mtime;
    __u32 stx_rdev_major;
    __u32 stx_rdev_minor;
    __u32 stx_dev_major;
    __u32 stx_dev_minor;
    __u64 stx_mnt_id;
    __u32 stx_dio_mem_align;
    __u32 stx_dio_offset_align;
    __u64 stx_subvol;
    __u32 stx_atomic_write_unit_min;
    __u32 stx_atomic_write_unit_max;
    __u32 stx_atomic_write_segments_max;
    __u32 stx_dio_read_offset_align;
    __u32 stx_atomic_write_unit_max_opt;
    __u32 __spare2[1];
    __u64 __spare3[8];
};

// statvfs: 0x48 (72) bytes
struct statvfs
{
    unsigned long f_bsize;
    unsigned long f_frsize;
    __fsblkcnt_t f_blocks;
    __fsblkcnt_t f_bfree;
    __fsblkcnt_t f_bavail;
    __fsfilcnt_t f_files;
    __fsfilcnt_t f_ffree;
    __fsfilcnt_t f_favail;
    unsigned long f_fsid;
    int __f_unused;
    unsigned long f_flag;
    unsigned long f_namemax;
    unsigned int f_type;
    int __f_spare[5];
};

// statvfs64: 0x60 (96) bytes
struct statvfs64
{
    unsigned long f_bsize;
    unsigned long f_frsize;
    __fsblkcnt64_t f_blocks;
    __fsblkcnt64_t f_bfree;
    __fsblkcnt64_t f_bavail;
    __fsfilcnt64_t f_files;
    __fsfilcnt64_t f_ffree;
    __fsfilcnt64_t f_favail;
    unsigned long f_fsid;
    int __f_unused;
    unsigned long f_flag;
    unsigned long f_namemax;
    unsigned int f_type;
    int __f_spare[5];
};

// tms: 0x10 (16) bytes
struct tms
{
    clock_t tms_utime;
    clock_t tms_stime;
    clock_t tms_cutime;
    clock_t tms_cstime;
};

// rusage: 0x48 (72) bytes
struct rusage
{
    struct timeval ru_utime;
    struct timeval ru_stime;
    union
    {
        long ru_maxrss;
        __syscall_slong_t __ru_maxrss_word;
    };
    union
    {
        long ru_ixrss;
        __syscall_slong_t __ru_ixrss_word;
    };
    union
    {
        long ru_idrss;
        __syscall_slong_t __ru_idrss_word;
    };
    union
    {
        long ru_isrss;
        __syscall_slong_t __ru_isrss_word;
    };
    union
    {
        long ru_minflt;
        __syscall_slong_t __ru_minflt_word;
    };
    union
    {
        long ru_majflt;
        __syscall_slong_t __ru_majflt_word;
    };
    union
    {
        long ru_nswap;
        __syscall_slong_t __ru_nswap_word;
    };
    union
    {
        long ru_inblock;
        __syscall_slong_t __ru_inblock_word;
    };
    union
    {
        long ru_oublock;
        __syscall_slong_t __ru_oublock_word;
    };
    union
    {
        long ru_msgsnd;
        __syscall_slong_t __ru_msgsnd_word;
    };
    union
    {
        long ru_msgrcv;
        __syscall_slong_t __ru_msgrcv_word;
    };
    union
    {
        long ru_nsignals;
        __syscall_slong_t __ru_nsignals_word;
    };
    union
    {
        long ru_nvcsw;
        __syscall_slong_t __ru_nvcsw_word;
    };
    union
    {
        long ru_nivcsw;
        __syscall_slong_t __ru_nivcsw_word;
    };
};

// winsize: 0x8 (8) bytes
struct winsize
{
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

// epoll_data: 0x8 (8) bytes
union epoll_data
{
    void *ptr;
    int fd;
    unsigned int u32;
    unsigned long long u64;
};

typedef union epoll_data epoll_data_t;

// epoll_event: 0xC (12) bytes, packed to 1
#pragma pack(push, 1)
struct epoll_event
{
    unsigned int events;
    epoll_data_t data;
};
#pragma pack(pop)

// epoll_params: 0x8 (8) bytes
struct epoll_params
{
    unsigned int busy_poll_usecs;
    unsigned short busy_poll_budget;
    unsigned char prefer_busy_poll;
    unsigned char __pad;
};

typedef unsigned long long eventfd_t;

// inotify_event: 0x10 (16) bytes
struct inotify_event
{
    int wd;
    unsigned int mask;
    unsigned int cookie;
    unsigned int len;
    char name[];
};

typedef __rlim_t rlim_t;

typedef __rlim64_t rlim64_t;

// rlimit: 0x8 (8) bytes
struct rlimit
{
    rlim_t rlim_cur;
    rlim_t rlim_max;
};

// rlimit64: 0x10 (16) bytes
struct rlimit64
{
    rlim64_t rlim_cur;
    rlim64_t rlim_max;
};

typedef enum __rlimit_resource __rlimit_resource_t;

typedef enum __rusage_who __rusage_who_t;

typedef enum __priority_which __priority_which_t;

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

// prctl_mm_map: 0x64 (100) bytes
struct prctl_mm_map
{
    __u64 start_code;
    __u64 end_code;
    __u64 start_data;
    __u64 end_data;
    __u64 start_brk;
    __u64 brk;
    __u64 start_stack;
    __u64 arg_start;
    __u64 arg_end;
    __u64 env_start;
    __u64 env_end;
    __u64 *auxv;
    __u32 auxv_size;
    __u32 exe_fd;
};

// __ptrace_peeksiginfo_args: 0x10 (16) bytes
struct __ptrace_peeksiginfo_args
{
    __uint64_t off;
    __uint32_t flags;
    __int32_t nr;
};

// __ptrace_seccomp_metadata: 0x10 (16) bytes
struct __ptrace_seccomp_metadata
{
    __uint64_t filter_off;
    __uint64_t flags;
};

// __ptrace_syscall_info: 0x54 (84) bytes
struct __ptrace_syscall_info
{
    __uint8_t op;
    __uint32_t arch;
    __uint64_t instruction_pointer;
    __uint64_t stack_pointer;
    union
    {
        struct
        {
            __uint64_t nr;
            __uint64_t args[6];
        } entry;
        struct
        {
            __int64_t rval;
            __uint8_t is_error;
        } exit;
        struct
        {
            __uint64_t nr;
            __uint64_t args[6];
            __uint32_t ret_data;
        } seccomp;
    };
};

// __ptrace_rseq_configuration: 0x18 (24) bytes
struct __ptrace_rseq_configuration
{
    __uint64_t rseq_abi_pointer;
    __uint32_t rseq_abi_size;
    __uint32_t signature;
    __uint32_t flags;
    __uint32_t pad;
};

// __ptrace_sud_config: 0x20 (32) bytes
struct __ptrace_sud_config
{
    __uint64_t mode;
    __uint64_t selector;
    __uint64_t offset;
    __uint64_t len;
};

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

// signalfd_siginfo: 0x80 (128) bytes
struct signalfd_siginfo
{
    unsigned int ssi_signo;
    int ssi_errno;
    int ssi_code;
    unsigned int ssi_pid;
    unsigned int ssi_uid;
    int ssi_fd;
    unsigned int ssi_tid;
    unsigned int ssi_band;
    unsigned int ssi_overrun;
    unsigned int ssi_trapno;
    int ssi_status;
    int ssi_int;
    unsigned long long ssi_ptr;
    unsigned long long ssi_utime;
    unsigned long long ssi_stime;
    unsigned long long ssi_addr;
    unsigned short ssi_addr_lsb;
    unsigned short __pad2;
    int ssi_syscall;
    unsigned long long ssi_call_addr;
    unsigned int ssi_arch;
    unsigned char __pad[28];
};

typedef unsigned short __ipc_pid_t;

// ipc_perm: 0x24 (36) bytes
struct ipc_perm
{
    __key_t __key;
    __uid_t uid;
    __gid_t gid;
    __uid_t cuid;
    __gid_t cgid;
    __mode_t mode;
    unsigned short __seq;
    unsigned short __pad2;
    __syscall_ulong_t __glibc_reserved1;
    __syscall_ulong_t __glibc_reserved2;
};

// semid_ds: 0x40 (64) bytes
struct semid_ds
{
    struct ipc_perm sem_perm;
    __time_t sem_otime;
    __syscall_ulong_t __sem_otime_high;
    __time_t sem_ctime;
    __syscall_ulong_t __sem_ctime_high;
    __syscall_ulong_t sem_nsems;
    __syscall_ulong_t __glibc_reserved3;
    __syscall_ulong_t __glibc_reserved4;
};

// __semid64_ds: 0x40 (64) bytes
struct __semid64_ds
{
    struct ipc_perm sem_perm;
    __time64_t sem_otime;
    __time64_t sem_ctime;
    __syscall_ulong_t sem_nsems;
    unsigned long __glibc_reserved3;
    unsigned long __glibc_reserved4;
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

// sembuf: 0x6 (6) bytes
struct sembuf
{
    unsigned short sem_num;
    short sem_op;
    short sem_flg;
};

typedef __syscall_ulong_t shmatt_t;

// shmid_ds: 0x54 (84) bytes
struct shmid_ds
{
    struct ipc_perm shm_perm;
    size_t shm_segsz;
    __time_t shm_atime;
    unsigned long __shm_atime_high;
    __time_t shm_dtime;
    unsigned long __shm_dtime_high;
    __time_t shm_ctime;
    unsigned long __shm_ctime_high;
    __pid_t shm_cpid;
    __pid_t shm_lpid;
    shmatt_t shm_nattch;
    __syscall_ulong_t __glibc_reserved5;
    __syscall_ulong_t __glibc_reserved6;
};

// __shmid64_ds: 0x54 (84) bytes
struct __shmid64_ds
{
    struct ipc_perm shm_perm;
    size_t shm_segsz;
    __time64_t shm_atime;
    __time64_t shm_dtime;
    __time64_t shm_ctime;
    __pid_t shm_cpid;
    __pid_t shm_lpid;
    shmatt_t shm_nattch;
    unsigned long __glibc_reserved5;
    unsigned long __glibc_reserved6;
};

// shminfo: 0x24 (36) bytes
struct shminfo
{
    __syscall_ulong_t shmmax;
    __syscall_ulong_t shmmin;
    __syscall_ulong_t shmmni;
    __syscall_ulong_t shmseg;
    __syscall_ulong_t shmall;
    __syscall_ulong_t __glibc_reserved1;
    __syscall_ulong_t __glibc_reserved2;
    __syscall_ulong_t __glibc_reserved3;
    __syscall_ulong_t __glibc_reserved4;
};

// shm_info: 0x18 (24) bytes
struct shm_info
{
    int used_ids;
    __syscall_ulong_t shm_tot;
    __syscall_ulong_t shm_rss;
    __syscall_ulong_t shm_swp;
    __syscall_ulong_t swap_attempts;
    __syscall_ulong_t swap_successes;
};

typedef __syscall_ulong_t msgqnum_t;

typedef __syscall_ulong_t msglen_t;

// msqid_ds: 0x58 (88) bytes
struct msqid_ds
{
    struct ipc_perm msg_perm;
    __time_t msg_stime;
    unsigned long __msg_stime_high;
    __time_t msg_rtime;
    unsigned long __msg_rtime_high;
    __time_t msg_ctime;
    unsigned long __msg_ctime_high;
    __syscall_ulong_t __msg_cbytes;
    msgqnum_t msg_qnum;
    msglen_t msg_qbytes;
    __pid_t msg_lspid;
    __pid_t msg_lrpid;
    __syscall_ulong_t __glibc_reserved4;
    __syscall_ulong_t __glibc_reserved5;
};

// __msqid64_ds: 0x58 (88) bytes
struct __msqid64_ds
{
    struct ipc_perm msg_perm;
    __time64_t msg_stime;
    __time64_t msg_rtime;
    __time64_t msg_ctime;
    __syscall_ulong_t __msg_cbytes;
    msgqnum_t msg_qnum;
    msglen_t msg_qbytes;
    __pid_t msg_lspid;
    __pid_t msg_lrpid;
    unsigned long __glibc_reserved4;
    unsigned long __glibc_reserved5;
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

// msgbuf: 0x8 (8) bytes
struct msgbuf
{
    __syscall_slong_t mtype;
    char mtext[1];
};

typedef unsigned int tcp_seq;

// tcphdr: 0x14 (20) bytes
struct tcphdr
{
    union
    {
        struct
        {
            unsigned short th_sport;
            unsigned short th_dport;
            tcp_seq th_seq;
            tcp_seq th_ack;
            unsigned char th_x2 : 4;
            unsigned char th_off : 4;
            unsigned char th_flags;
            unsigned short th_win;
            unsigned short th_sum;
            unsigned short th_urp;
        };
        struct
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

// tcp_info: 0x118 (280) bytes
struct tcp_info
{
    unsigned char tcpi_state;
    unsigned char tcpi_ca_state;
    unsigned char tcpi_retransmits;
    unsigned char tcpi_probes;
    unsigned char tcpi_backoff;
    unsigned char tcpi_options;
    unsigned char tcpi_snd_wscale : 4;
    unsigned char tcpi_rcv_wscale : 4;
    unsigned int tcpi_rto;
    unsigned int tcpi_ato;
    unsigned int tcpi_snd_mss;
    unsigned int tcpi_rcv_mss;
    unsigned int tcpi_unacked;
    unsigned int tcpi_sacked;
    unsigned int tcpi_lost;
    unsigned int tcpi_retrans;
    unsigned int tcpi_fackets;
    unsigned int tcpi_last_data_sent;
    unsigned int tcpi_last_ack_sent;
    unsigned int tcpi_last_data_recv;
    unsigned int tcpi_last_ack_recv;
    unsigned int tcpi_pmtu;
    unsigned int tcpi_rcv_ssthresh;
    unsigned int tcpi_rtt;
    unsigned int tcpi_rttvar;
    unsigned int tcpi_snd_ssthresh;
    unsigned int tcpi_snd_cwnd;
    unsigned int tcpi_advmss;
    unsigned int tcpi_reordering;
    unsigned int tcpi_rcv_rtt;
    unsigned int tcpi_rcv_space;
    unsigned int tcpi_total_retrans;
    unsigned long long tcpi_pacing_rate;
    unsigned long long tcpi_max_pacing_rate;
    unsigned long long tcpi_bytes_acked;
    unsigned long long tcpi_bytes_received;
    unsigned int tcpi_segs_out;
    unsigned int tcpi_segs_in;
    unsigned int tcpi_notsent_bytes;
    unsigned int tcpi_min_rtt;
    unsigned int tcpi_data_segs_in;
    unsigned int tcpi_data_segs_out;
    unsigned long long tcpi_delivery_rate;
    unsigned long long tcpi_busy_time;
    unsigned long long tcpi_rwnd_limited;
    unsigned long long tcpi_sndbuf_limited;
    unsigned int tcpi_delivered;
    unsigned int tcpi_delivered_ce;
    unsigned long long tcpi_bytes_sent;
    unsigned long long tcpi_bytes_retrans;
    unsigned int tcpi_dsack_dups;
    unsigned int tcpi_reord_seen;
    unsigned int tcpi_rcv_ooopack;
    unsigned int tcpi_snd_wnd;
    unsigned int tcpi_rcv_wnd;
    unsigned int tcpi_rehash;
    unsigned short tcpi_total_rto;
    unsigned short tcpi_total_rto_recoveries;
    unsigned int tcpi_total_rto_time;
    unsigned int tcpi_received_ce;
    unsigned int tcpi_delivered_e1_bytes;
    unsigned int tcpi_delivered_e0_bytes;
    unsigned int tcpi_delivered_ce_bytes;
    unsigned int tcpi_received_e1_bytes;
    unsigned int tcpi_received_e0_bytes;
    unsigned int tcpi_received_ce_bytes;
    unsigned short tcpi_accecn_fail_mode;
    unsigned short tcpi_accecn_opt_seen;
};

// tcp_md5sig: 0xD8 (216) bytes
struct tcp_md5sig
{
    struct sockaddr_storage tcpm_addr;
    unsigned char tcpm_flags;
    unsigned char tcpm_prefixlen;
    unsigned short tcpm_keylen;
    int tcpm_ifindex;
    unsigned char tcpm_key[80];
};

// tcp_diag_md5sig: 0x64 (100) bytes
struct tcp_diag_md5sig
{
    unsigned char tcpm_family;
    unsigned char tcpm_prefixlen;
    unsigned short tcpm_keylen;
    unsigned int tcpm_addr[4];
    unsigned char tcpm_key[80];
};

// tcp_ao_add: 0x120 (288) bytes
struct DECLSPEC_ALIGN(8) tcp_ao_add
{
    struct sockaddr_storage addr;
    signed char alg_name[64];
    int ifindex;
    unsigned int set_current : 1;
    unsigned int set_rnext : 1;
    unsigned int reserved : 30;
    unsigned short reserved2;
    unsigned char prefix;
    unsigned char sndid;
    unsigned char rcvid;
    unsigned char maclen;
    unsigned char keyflags;
    unsigned char keylen;
    unsigned char key[80];
};

// tcp_ao_del: 0x90 (144) bytes
struct DECLSPEC_ALIGN(8) tcp_ao_del
{
    struct sockaddr_storage addr;
    int ifindex;
    unsigned int set_current : 1;
    unsigned int set_rnext : 1;
    unsigned int del_async : 1;
    unsigned int reserved : 29;
    unsigned short reserved2;
    unsigned char prefix;
    unsigned char sndid;
    unsigned char rcvid;
    unsigned char current_key;
    unsigned char rnext;
    unsigned char keyflags;
};

// tcp_ao_info_opt: 0x30 (48) bytes
struct DECLSPEC_ALIGN(8) tcp_ao_info_opt
{
    unsigned int set_current : 1;
    unsigned int set_rnext : 1;
    unsigned int ao_required : 1;
    unsigned int set_counters : 1;
    unsigned int accept_icmps : 1;
    unsigned int reserved : 27;
    unsigned short reserved2;
    unsigned char current_key;
    unsigned char rnext;
    unsigned long long pkt_good;
    unsigned long long pkt_bad;
    unsigned long long pkt_key_not_found;
    unsigned long long pkt_ao_required;
    unsigned long long pkt_dropped_icmp;
};

// tcp_ao_getsockopt: 0x130 (304) bytes
struct DECLSPEC_ALIGN(8) tcp_ao_getsockopt
{
    struct sockaddr_storage addr;
    unsigned char alg_name[64];
    unsigned char key[80];
    unsigned int nkeys;
    unsigned short is_current : 1;
    unsigned short is_rnext : 1;
    unsigned short get_all : 1;
    unsigned short reserved : 13;
    unsigned char sndid;
    unsigned char rcvid;
    unsigned char prefix;
    unsigned char maclen;
    unsigned char keyflags;
    unsigned char keylen;
    int ifindex;
    unsigned long long pkt_good;
    unsigned long long pkt_bad;
};

// tcp_ao_repair: 0x10 (16) bytes
struct DECLSPEC_ALIGN(8) tcp_ao_repair
{
    unsigned int snt_isn;
    unsigned int rcv_isn;
    unsigned int snd_sne;
    unsigned int rcv_sne;
};

// tcp_repair_opt: 0x8 (8) bytes
struct tcp_repair_opt
{
    unsigned int opt_code;
    unsigned int opt_val;
};

// tcp_cookie_transactions: 0x220 (544) bytes
struct tcp_cookie_transactions
{
    unsigned short tcpct_flags;
    unsigned char __tcpct_pad1;
    unsigned char tcpct_cookie_desired;
    unsigned short tcpct_s_data_desired;
    unsigned short tcpct_used;
    unsigned char tcpct_value[536];
};

// tcp_repair_window: 0x14 (20) bytes
struct tcp_repair_window
{
    unsigned int snd_wl1;
    unsigned int snd_wnd;
    unsigned int max_window;
    unsigned int rcv_wnd;
    unsigned int rcv_wup;
};

// tcp_zerocopy_receive: 0x40 (64) bytes
struct tcp_zerocopy_receive
{
    unsigned long long address;
    unsigned int length;
    unsigned int recv_skip_hint;
    unsigned int inq;
    int err;
    unsigned long long copybuf_address;
    int copybuf_len;
    unsigned int flags;
    unsigned long long msg_control;
    unsigned long long msg_controllen;
    unsigned int msg_flags;
    unsigned int reserved;
};

// if_nameindex: 0x8 (8) bytes
struct if_nameindex
{
    unsigned int if_index;
    char *if_name;
};

// Declared only: its members are unknown.
struct iface;

// ifaddr: 0x28 (40) bytes
struct ifaddr
{
    struct sockaddr ifa_addr;
    union
    {
        struct sockaddr ifu_broadaddr;
        struct sockaddr ifu_dstaddr;
    } ifa_ifu;
    struct iface *ifa_ifp;
    struct ifaddr *ifa_next;
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
        __caddr_t ifru_data;
    } ifr_ifru;
};

// ifconf: 0x8 (8) bytes
struct ifconf
{
    int ifc_len;
    union
    {
        __caddr_t ifcu_buf;
        struct ifreq *ifcu_req;
    } ifc_ifcu;
};

// aiocb: 0x90 (144) bytes
struct aiocb
{
    int aio_fildes;
    int aio_lio_opcode;
    int aio_reqprio;
    volatile void *aio_buf;
    size_t aio_nbytes;
    struct sigevent aio_sigevent;
    struct aiocb *__next_prio;
    int __abs_prio;
    int __policy;
    int __error_code;
    __ssize_t __return_value;
    __off_t aio_offset;
    char __pad[4];
    char __glibc_reserved[32];
};

// aiocb64: 0x90 (144) bytes
struct aiocb64
{
    int aio_fildes;
    int aio_lio_opcode;
    int aio_reqprio;
    volatile void *aio_buf;
    size_t aio_nbytes;
    struct sigevent aio_sigevent;
    struct aiocb *__next_prio;
    int __abs_prio;
    int __policy;
    int __error_code;
    __ssize_t __return_value;
    __off64_t aio_offset;
    char __glibc_reserved[32];
};

// aioinit: 0x20 (32) bytes
struct aioinit
{
    int aio_threads;
    int aio_num;
    int aio_locks;
    int aio_usedba;
    int aio_debug;
    int aio_numusers;
    int aio_idle_time;
    int aio_reserved;
};

typedef int mqd_t;

// mq_attr: 0x20 (32) bytes
struct mq_attr
{
    __syscall_slong_t mq_flags;
    __syscall_slong_t mq_maxmsg;
    __syscall_slong_t mq_msgsize;
    __syscall_slong_t mq_curmsgs;
    __syscall_slong_t __pad[4];
};

// mallinfo: 0x28 (40) bytes
struct mallinfo
{
    int arena;
    int ordblks;
    int smblks;
    int hblks;
    int hblkhd;
    int usmblks;
    int fsmblks;
    int uordblks;
    int fordblks;
    int keepcost;
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
    unsigned short fts_pathlen;
    unsigned short fts_namelen;
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

// FTS: 0x2C (44) bytes
struct FTS
{
    struct _ftsent *fts_cur;
    struct _ftsent *fts_child;
    struct _ftsent **fts_array;
    dev_t fts_dev;
    char *fts_path;
    int fts_rfd;
    int fts_pathlen;
    int fts_nitems;
    int (*fts_compar)(const void *, const void *);
    int fts_options;
};

// _ftsent64: 0x4C (76) bytes
struct _ftsent64
{
    struct _ftsent64 *fts_cycle;
    struct _ftsent64 *fts_parent;
    struct _ftsent64 *fts_link;
    long fts_number;
    void *fts_pointer;
    char *fts_accpath;
    char *fts_path;
    int fts_errno;
    int fts_symfd;
    unsigned short fts_pathlen;
    unsigned short fts_namelen;
    ino64_t fts_ino;
    dev_t fts_dev;
    nlink_t fts_nlink;
    short fts_level;
    unsigned short fts_info;
    unsigned short fts_flags;
    unsigned short fts_instr;
    struct stat64 *fts_statp;
    char fts_name[1];
};

// FTS64: 0x2C (44) bytes
struct FTS64
{
    struct _ftsent64 *fts_cur;
    struct _ftsent64 *fts_child;
    struct _ftsent64 **fts_array;
    dev_t fts_dev;
    char *fts_path;
    int fts_rfd;
    int fts_pathlen;
    int fts_nitems;
    int (*fts_compar)(const void *, const void *);
    int fts_options;
};

typedef struct _ftsent FTSENT;

typedef struct _ftsent64 FTSENT64;

// FTW: 0x8 (8) bytes
struct FTW
{
    int base;
    int level;
};

typedef int (*__ftw_func_t)(const char *__filename, const struct stat *__status, int __flag);

typedef int (*__ftw64_func_t)(const char *__filename, const struct stat64 *__status, int __flag);

typedef int (*__nftw_func_t)(const char *__filename, const struct stat *__status, int __flag, struct FTW *__info);

typedef int (*__nftw64_func_t)(const char *__filename, const struct stat64 *__status, int __flag, struct FTW *__info);

// wordexp_t: 0xC (12) bytes
struct wordexp_t
{
    size_t we_wordc;
    char **we_wordv;
    size_t we_offs;
};

// spwd: 0x24 (36) bytes
struct spwd
{
    char *sp_namp;
    char *sp_pwdp;
    long sp_lstchg;
    long sp_min;
    long sp_max;
    long sp_warn;
    long sp_inact;
    long sp_expire;
    unsigned long sp_flag;
};


_Noreturn void _Exit(int __status);
__pid_t _Fork(void);
double __acos(double __x);
float __acosf(float __x);
_Float32 __acosf32(_Float32 __x);
_Float32x __acosf32x(_Float32x __x);
_Float64 __acosf64(_Float64 __x);
_Float64x __acosf64x(_Float64x __x);
double __acosh(double __x);
float __acoshf(float __x);
_Float32 __acoshf32(_Float32 __x);
_Float32x __acoshf32x(_Float32x __x);
_Float64 __acoshf64(_Float64 __x);
_Float64x __acoshf64x(_Float64x __x);
long double __acoshl(long double __x);
long double __acosl(long double __x);
double __acospi(double __x);
float __acospif(float __x);
_Float32 __acospif32(_Float32 __x);
_Float32x __acospif32x(_Float32x __x);
_Float64 __acospif64(_Float64 __x);
_Float64x __acospif64x(_Float64x __x);
long double __acospil(long double __x);
double __asin(double __x);
float __asinf(float __x);
_Float32 __asinf32(_Float32 __x);
_Float32x __asinf32x(_Float32x __x);
_Float64 __asinf64(_Float64 __x);
_Float64x __asinf64x(_Float64x __x);
double __asinh(double __x);
float __asinhf(float __x);
_Float32 __asinhf32(_Float32 __x);
_Float32x __asinhf32x(_Float32x __x);
_Float64 __asinhf64(_Float64 __x);
_Float64x __asinhf64x(_Float64x __x);
long double __asinhl(long double __x);
long double __asinl(long double __x);
double __asinpi(double __x);
float __asinpif(float __x);
_Float32 __asinpif32(_Float32 __x);
_Float32x __asinpif32x(_Float32x __x);
_Float64 __asinpif64(_Float64 __x);
_Float64x __asinpif64x(_Float64x __x);
long double __asinpil(long double __x);
int __asprintf(char **__ptr, const char *__fmt, ...);
_Noreturn void __assert(const char *__assertion, const char *__file, int __line);
_Noreturn void __assert_fail(const char *__assertion, const char *__file, unsigned int __line, const char *__function);
_Noreturn void __assert_perror_fail(int __errnum, const char *__file, unsigned int __line, const char *__function);
bool __assert_single_arg(bool);
double __atan(double __x);
double __atan2(double __y, double __x);
float __atan2f(float __y, float __x);
_Float32 __atan2f32(_Float32 __y, _Float32 __x);
_Float32x __atan2f32x(_Float32x __y, _Float32x __x);
_Float64 __atan2f64(_Float64 __y, _Float64 __x);
_Float64x __atan2f64x(_Float64x __y, _Float64x __x);
long double __atan2l(long double __y, long double __x);
double __atan2pi(double __y, double __x);
float __atan2pif(float __y, float __x);
_Float32 __atan2pif32(_Float32 __y, _Float32 __x);
_Float32x __atan2pif32x(_Float32x __y, _Float32x __x);
_Float64 __atan2pif64(_Float64 __y, _Float64 __x);
_Float64x __atan2pif64x(_Float64x __y, _Float64x __x);
long double __atan2pil(long double __y, long double __x);
float __atanf(float __x);
_Float32 __atanf32(_Float32 __x);
_Float32x __atanf32x(_Float32x __x);
_Float64 __atanf64(_Float64 __x);
_Float64x __atanf64x(_Float64x __x);
double __atanh(double __x);
float __atanhf(float __x);
_Float32 __atanhf32(_Float32 __x);
_Float32x __atanhf32x(_Float32x __x);
_Float64 __atanhf64(_Float64 __x);
_Float64x __atanhf64x(_Float64x __x);
long double __atanhl(long double __x);
long double __atanl(long double __x);
double __atanpi(double __x);
float __atanpif(float __x);
_Float32 __atanpif32(_Float32 __x);
_Float32x __atanpif32x(_Float32x __x);
_Float64 __atanpif64(_Float64 __x);
_Float64x __atanpif64x(_Float64x __x);
long double __atanpil(long double __x);
__uint16_t __bswap_16(__uint16_t __bsx);
__uint32_t __bswap_32(__uint32_t __bsx);
__uint64_t __bswap_64(__uint64_t __bsx);
double __cbrt(double __x);
float __cbrtf(float __x);
_Float32 __cbrtf32(_Float32 __x);
_Float32x __cbrtf32x(_Float32x __x);
_Float64 __cbrtf64(_Float64 __x);
_Float64x __cbrtf64x(_Float64x __x);
long double __cbrtl(long double __x);
struct cmsghdr *__cmsg_nxthdr(struct msghdr *__mhdr, struct cmsghdr *__cmsg);
double __compoundn(double __x, long long __y);
float __compoundnf(float __x, long long __y);
_Float32 __compoundnf32(_Float32 __x, long long __y);
_Float32x __compoundnf32x(_Float32x __x, long long __y);
_Float64 __compoundnf64(_Float64 __x, long long __y);
_Float64x __compoundnf64x(_Float64x __x, long long __y);
long double __compoundnl(long double __x, long long __y);
double __cos(double __x);
float __cosf(float __x);
_Float32 __cosf32(_Float32 __x);
_Float32x __cosf32x(_Float32x __x);
_Float64 __cosf64(_Float64 __x);
_Float64x __cosf64x(_Float64x __x);
double __cosh(double __x);
float __coshf(float __x);
_Float32 __coshf32(_Float32 __x);
_Float32x __coshf32x(_Float32x __x);
_Float64 __coshf64(_Float64 __x);
_Float64x __coshf64x(_Float64x __x);
long double __coshl(long double __x);
long double __cosl(long double __x);
double __cospi(double __x);
float __cospif(float __x);
_Float32 __cospif32(_Float32 __x);
_Float32x __cospif32x(_Float32x __x);
_Float64 __cospif64(_Float64 __x);
_Float64x __cospif64x(_Float64x __x);
long double __cospil(long double __x);
const unsigned short **__ctype_b_loc(void);
size_t __ctype_get_mb_cur_max(void);
const __int32_t **__ctype_tolower_loc(void);
const __int32_t **__ctype_toupper_loc(void);
double __drem(double __x, double __y);
float __dremf(float __x, float __y);
long double __dreml(long double __x, long double __y);
double __erf(double);
double __erfc(double);
float __erfcf(float);
_Float32 __erfcf32(_Float32);
_Float32x __erfcf32x(_Float32x);
_Float64 __erfcf64(_Float64);
_Float64x __erfcf64x(_Float64x);
long double __erfcl(long double);
float __erff(float);
_Float32 __erff32(_Float32);
_Float32x __erff32x(_Float32x);
_Float64 __erff64(_Float64);
_Float64x __erff64x(_Float64x);
long double __erfl(long double);
int *__errno_location(void);
double __exp(double __x);
double __exp10(double __x);
float __exp10f(float __x);
_Float32 __exp10f32(_Float32 __x);
_Float32x __exp10f32x(_Float32x __x);
_Float64 __exp10f64(_Float64 __x);
_Float64x __exp10f64x(_Float64x __x);
long double __exp10l(long double __x);
double __exp10m1(double __x);
float __exp10m1f(float __x);
_Float32 __exp10m1f32(_Float32 __x);
_Float32x __exp10m1f32x(_Float32x __x);
_Float64 __exp10m1f64(_Float64 __x);
_Float64x __exp10m1f64x(_Float64x __x);
long double __exp10m1l(long double __x);
double __exp2(double __x);
float __exp2f(float __x);
_Float32 __exp2f32(_Float32 __x);
_Float32x __exp2f32x(_Float32x __x);
_Float64 __exp2f64(_Float64 __x);
_Float64x __exp2f64x(_Float64x __x);
long double __exp2l(long double __x);
double __exp2m1(double __x);
float __exp2m1f(float __x);
_Float32 __exp2m1f32(_Float32 __x);
_Float32x __exp2m1f32x(_Float32x __x);
_Float64 __exp2m1f64(_Float64 __x);
_Float64x __exp2m1f64x(_Float64x __x);
long double __exp2m1l(long double __x);
float __expf(float __x);
_Float32 __expf32(_Float32 __x);
_Float32x __expf32x(_Float32x __x);
_Float64 __expf64(_Float64 __x);
_Float64x __expf64x(_Float64x __x);
long double __expl(long double __x);
double __expm1(double __x);
float __expm1f(float __x);
_Float32 __expm1f32(_Float32 __x);
_Float32x __expm1f32x(_Float32x __x);
_Float64 __expm1f64(_Float64 __x);
_Float64x __expm1f64x(_Float64x __x);
long double __expm1l(long double __x);
double __fdim(double __x, double __y);
float __fdimf(float __x, float __y);
_Float32 __fdimf32(_Float32 __x, _Float32 __y);
_Float32x __fdimf32x(_Float32x __x, _Float32x __y);
_Float64 __fdimf64(_Float64 __x, _Float64 __y);
_Float64x __fdimf64x(_Float64x __x, _Float64x __y);
long double __fdiml(long double __x, long double __y);
int __finite(double __value);
int __finitef(float __value);
int __finitel(long double __value);
double __fma(double __x, double __y, double __z);
float __fmaf(float __x, float __y, float __z);
_Float32 __fmaf32(_Float32 __x, _Float32 __y, _Float32 __z);
_Float32x __fmaf32x(_Float32x __x, _Float32x __y, _Float32x __z);
_Float64 __fmaf64(_Float64 __x, _Float64 __y, _Float64 __z);
_Float64x __fmaf64x(_Float64x __x, _Float64x __y, _Float64x __z);
long double __fmal(long double __x, long double __y, long double __z);
double __fmod(double __x, double __y);
float __fmodf(float __x, float __y);
_Float32 __fmodf32(_Float32 __x, _Float32 __y);
_Float32x __fmodf32x(_Float32x __x, _Float32x __y);
_Float64 __fmodf64(_Float64 __x, _Float64 __y);
_Float64x __fmodf64x(_Float64x __x, _Float64x __y);
long double __fmodl(long double __x, long double __y);
int __fpclassify(double __value);
int __fpclassifyf(float __value);
int __fpclassifyl(long double __value);
double __frexp(double __x, int *__exponent);
float __frexpf(float __x, int *__exponent);
_Float32 __frexpf32(_Float32 __x, int *__exponent);
_Float32x __frexpf32x(_Float32x __x, int *__exponent);
_Float64 __frexpf64(_Float64 __x, int *__exponent);
_Float64x __frexpf64x(_Float64x __x, int *__exponent);
long double __frexpl(long double __x, int *__exponent);
double __fromfp(double __x, int __round, unsigned int __width);
float __fromfpf(float __x, int __round, unsigned int __width);
_Float32 __fromfpf32(_Float32 __x, int __round, unsigned int __width);
_Float32x __fromfpf32x(_Float32x __x, int __round, unsigned int __width);
_Float64 __fromfpf64(_Float64 __x, int __round, unsigned int __width);
_Float64x __fromfpf64x(_Float64x __x, int __round, unsigned int __width);
long double __fromfpl(long double __x, int __round, unsigned int __width);
double __fromfpx(double __x, int __round, unsigned int __width);
float __fromfpxf(float __x, int __round, unsigned int __width);
_Float32 __fromfpxf32(_Float32 __x, int __round, unsigned int __width);
_Float32x __fromfpxf32x(_Float32x __x, int __round, unsigned int __width);
_Float64 __fromfpxf64(_Float64 __x, int __round, unsigned int __width);
_Float64x __fromfpxf64x(_Float64x __x, int __round, unsigned int __width);
long double __fromfpxl(long double __x, int __round, unsigned int __width);
double __gamma(double);
float __gammaf(float);
long double __gammal(long double);
__ssize_t __getdelim(char **__lineptr, size_t *__n, int __delimiter, FILE *__stream);
int __getpagesize(void);
double __getpayload(const double *__x);
float __getpayloadf(const float *__x);
_Float32 __getpayloadf32(const _Float32 *__x);
_Float32x __getpayloadf32x(const _Float32x *__x);
_Float64 __getpayloadf64(const _Float64 *__x);
_Float64x __getpayloadf64x(const _Float64x *__x);
long double __getpayloadl(const long double *__x);
__pid_t __getpgid(__pid_t __pid);
int *__h_errno_location(void);
double __hypot(double __x, double __y);
float __hypotf(float __x, float __y);
_Float32 __hypotf32(_Float32 __x, _Float32 __y);
_Float32x __hypotf32x(_Float32x __x, _Float32x __y);
_Float64 __hypotf64(_Float64 __x, _Float64 __y);
_Float64x __hypotf64x(_Float64x __x, _Float64x __y);
long double __hypotl(long double __x, long double __y);
int __ilogb(double __x);
int __ilogbf(float __x);
int __ilogbf32(_Float32 __x);
int __ilogbf32x(_Float32x __x);
int __ilogbf64(_Float64 __x);
int __ilogbf64x(_Float64x __x);
int __ilogbl(long double __x);
int __iscanonicall(long double __x);
int __iseqsig(double __x, double __y);
int __iseqsigf(float __x, float __y);
int __iseqsigl(long double __x, long double __y);
int __isinf(double __value);
int __isinff(float __value);
int __isinfl(long double __value);
int __isnan(double __value);
int __isnanf(float __value);
int __isnanl(long double __value);
int __isoc23_fscanf(FILE *__stream, const char *__format, ...);
int __isoc23_fwscanf(__FILE *__stream, const int *__format, ...);
int __isoc23_scanf(const char *__format, ...);
int __isoc23_sscanf(const char *__s, const char *__format, ...);
intmax_t __isoc23_strtoimax(const char *__nptr, char **__endptr, int __base);
long __isoc23_strtol(const char *__nptr, char **__endptr, int __base);
long __isoc23_strtol_l(const char *__nptr, char **__endptr, int __base, locale_t __loc);
long long __isoc23_strtoll(const char *__nptr, char **__endptr, int __base);
long long __isoc23_strtoll_l(const char *__nptr, char **__endptr, int __base, locale_t __loc);
unsigned long __isoc23_strtoul(const char *__nptr, char **__endptr, int __base);
unsigned long __isoc23_strtoul_l(const char *__nptr, char **__endptr, int __base, locale_t __loc);
unsigned long long __isoc23_strtoull(const char *__nptr, char **__endptr, int __base);
unsigned long long __isoc23_strtoull_l(const char *__nptr, char **__endptr, int __base, locale_t __loc);
uintmax_t __isoc23_strtoumax(const char *__nptr, char **__endptr, int __base);
int __isoc23_swscanf(const int *__s, const int *__format, ...);
int __isoc23_vfscanf(FILE *__s, const char *__format, char *__arg);
int __isoc23_vfwscanf(__FILE *__s, const int *__format, char *__arg);
int __isoc23_vscanf(const char *__format, char *__arg);
int __isoc23_vsscanf(const char *__s, const char *__format, char *__arg);
int __isoc23_vswscanf(const int *__s, const int *__format, char *__arg);
int __isoc23_vwscanf(const int *__format, char *__arg);
intmax_t __isoc23_wcstoimax(const __gwchar_t *__nptr, __gwchar_t **__endptr, int __base);
long __isoc23_wcstol(const int *__nptr, int **__endptr, int __base);
long __isoc23_wcstol_l(const int *__nptr, int **__endptr, int __base, locale_t __loc);
long long __isoc23_wcstoll(const int *__nptr, int **__endptr, int __base);
long long __isoc23_wcstoll_l(const int *__nptr, int **__endptr, int __base, locale_t __loc);
unsigned long __isoc23_wcstoul(const int *__nptr, int **__endptr, int __base);
unsigned long __isoc23_wcstoul_l(const int *__nptr, int **__endptr, int __base, locale_t __loc);
unsigned long long __isoc23_wcstoull(const int *__nptr, int **__endptr, int __base);
unsigned long long __isoc23_wcstoull_l(const int *__nptr, int **__endptr, int __base, locale_t __loc);
uintmax_t __isoc23_wcstoumax(const __gwchar_t *__nptr, __gwchar_t **__endptr, int __base);
int __isoc23_wscanf(const int *__format, ...);
int __issignaling(double __value);
int __issignalingf(float __value);
int __issignalingl(long double __value);
double __j0(double);
float __j0f(float);
_Float32 __j0f32(_Float32);
_Float32x __j0f32x(_Float32x);
_Float64 __j0f64(_Float64);
_Float64x __j0f64x(_Float64x);
long double __j0l(long double);
double __j1(double);
float __j1f(float);
_Float32 __j1f32(_Float32);
_Float32x __j1f32x(_Float32x);
_Float64 __j1f64(_Float64);
_Float64x __j1f64x(_Float64x);
long double __j1l(long double);
double __jn(int, double);
float __jnf(int, float);
_Float32 __jnf32(int, _Float32);
_Float32x __jnf32x(int, _Float32x);
_Float64 __jnf64(int, _Float64);
_Float64x __jnf64x(int, _Float64x);
long double __jnl(int, long double);
double __ldexp(double __x, int __exponent);
float __ldexpf(float __x, int __exponent);
_Float32 __ldexpf32(_Float32 __x, int __exponent);
_Float32x __ldexpf32x(_Float32x __x, int __exponent);
_Float64 __ldexpf64(_Float64 __x, int __exponent);
_Float64x __ldexpf64x(_Float64x __x, int __exponent);
long double __ldexpl(long double __x, int __exponent);
double __lgamma(double);
double __lgamma_r(double, int *__signgamp);
float __lgammaf(float);
_Float32 __lgammaf32(_Float32);
_Float32 __lgammaf32_r(_Float32, int *__signgamp);
_Float32x __lgammaf32x(_Float32x);
_Float32x __lgammaf32x_r(_Float32x, int *__signgamp);
_Float64 __lgammaf64(_Float64);
_Float64 __lgammaf64_r(_Float64, int *__signgamp);
_Float64x __lgammaf64x(_Float64x);
_Float64x __lgammaf64x_r(_Float64x, int *__signgamp);
float __lgammaf_r(float, int *__signgamp);
long double __lgammal(long double);
long double __lgammal_r(long double, int *__signgamp);
int __libc_current_sigrtmax(void);
int __libc_current_sigrtmin(void);
long __llogb(double __x);
long __llogbf(float __x);
long __llogbf32(_Float32 __x);
long __llogbf32x(_Float32x __x);
long __llogbf64(_Float64 __x);
long __llogbf64x(_Float64x __x);
long __llogbl(long double __x);
long long __llrint(double __x);
long long __llrintf(float __x);
long long __llrintf32(_Float32 __x);
long long __llrintf32x(_Float32x __x);
long long __llrintf64(_Float64 __x);
long long __llrintf64x(_Float64x __x);
long long __llrintl(long double __x);
long long __llround(double __x);
long long __llroundf(float __x);
long long __llroundf32(_Float32 __x);
long long __llroundf32x(_Float32x __x);
long long __llroundf64(_Float64 __x);
long long __llroundf64x(_Float64x __x);
long long __llroundl(long double __x);
double __log(double __x);
double __log10(double __x);
float __log10f(float __x);
_Float32 __log10f32(_Float32 __x);
_Float32x __log10f32x(_Float32x __x);
_Float64 __log10f64(_Float64 __x);
_Float64x __log10f64x(_Float64x __x);
long double __log10l(long double __x);
double __log10p1(double __x);
float __log10p1f(float __x);
_Float32 __log10p1f32(_Float32 __x);
_Float32x __log10p1f32x(_Float32x __x);
_Float64 __log10p1f64(_Float64 __x);
_Float64x __log10p1f64x(_Float64x __x);
long double __log10p1l(long double __x);
double __log1p(double __x);
float __log1pf(float __x);
_Float32 __log1pf32(_Float32 __x);
_Float32x __log1pf32x(_Float32x __x);
_Float64 __log1pf64(_Float64 __x);
_Float64x __log1pf64x(_Float64x __x);
long double __log1pl(long double __x);
double __log2(double __x);
float __log2f(float __x);
_Float32 __log2f32(_Float32 __x);
_Float32x __log2f32x(_Float32x __x);
_Float64 __log2f64(_Float64 __x);
_Float64x __log2f64x(_Float64x __x);
long double __log2l(long double __x);
double __log2p1(double __x);
float __log2p1f(float __x);
_Float32 __log2p1f32(_Float32 __x);
_Float32x __log2p1f32x(_Float32x __x);
_Float64 __log2p1f64(_Float64 __x);
_Float64x __log2p1f64x(_Float64x __x);
long double __log2p1l(long double __x);
double __logb(double __x);
float __logbf(float __x);
_Float32 __logbf32(_Float32 __x);
_Float32x __logbf32x(_Float32x __x);
_Float64 __logbf64(_Float64 __x);
_Float64x __logbf64x(_Float64x __x);
long double __logbl(long double __x);
float __logf(float __x);
_Float32 __logf32(_Float32 __x);
_Float32x __logf32x(_Float32x __x);
_Float64 __logf64(_Float64 __x);
_Float64x __logf64x(_Float64x __x);
long double __logl(long double __x);
double __logp1(double __x);
float __logp1f(float __x);
_Float32 __logp1f32(_Float32 __x);
_Float32x __logp1f32x(_Float32x __x);
_Float64 __logp1f64(_Float64 __x);
_Float64x __logp1f64x(_Float64x __x);
long double __logp1l(long double __x);
long __lrint(double __x);
long __lrintf(float __x);
long __lrintf32(_Float32 __x);
long __lrintf32x(_Float32x __x);
long __lrintf64(_Float64 __x);
long __lrintf64x(_Float64x __x);
long __lrintl(long double __x);
long __lround(double __x);
long __lroundf(float __x);
long __lroundf32(_Float32 __x);
long __lroundf32x(_Float32x __x);
long __lroundf64(_Float64 __x);
long __lroundf64x(_Float64x __x);
long __lroundl(long double __x);
size_t __mbrlen(const char *__s, size_t __n, mbstate_t *__ps);
int __memcmpeq(const void *__s1, const void *__s2, size_t __n);
void *__mempcpy(void *__dest, const void *__src, size_t __n);
double __modf(double __x, double *__iptr);
float __modff(float __x, float *__iptr);
_Float32 __modff32(_Float32 __x, _Float32 *__iptr);
_Float32x __modff32x(_Float32x __x, _Float32x *__iptr);
_Float64 __modff64(_Float64 __x, _Float64 *__iptr);
_Float64x __modff64x(_Float64x __x, _Float64x *__iptr);
long double __modfl(long double __x, long double *__iptr);
double __nan(const char *__tagb);
float __nanf(const char *__tagb);
_Float32 __nanf32(const char *__tagb);
_Float32x __nanf32x(const char *__tagb);
_Float64 __nanf64(const char *__tagb);
_Float64x __nanf64x(const char *__tagb);
long double __nanl(const char *__tagb);
double __nearbyint(double __x);
float __nearbyintf(float __x);
_Float32 __nearbyintf32(_Float32 __x);
_Float32x __nearbyintf32x(_Float32x __x);
_Float64 __nearbyintf64(_Float64 __x);
_Float64x __nearbyintf64x(_Float64x __x);
long double __nearbyintl(long double __x);
double __nextafter(double __x, double __y);
float __nextafterf(float __x, float __y);
_Float32 __nextafterf32(_Float32 __x, _Float32 __y);
_Float32x __nextafterf32x(_Float32x __x, _Float32x __y);
_Float64 __nextafterf64(_Float64 __x, _Float64 __y);
_Float64x __nextafterf64x(_Float64x __x, _Float64x __y);
long double __nextafterl(long double __x, long double __y);
double __nextdown(double __x);
float __nextdownf(float __x);
_Float32 __nextdownf32(_Float32 __x);
_Float32x __nextdownf32x(_Float32x __x);
_Float64 __nextdownf64(_Float64 __x);
_Float64x __nextdownf64x(_Float64x __x);
long double __nextdownl(long double __x);
double __nexttoward(double __x, long double __y);
float __nexttowardf(float __x, long double __y);
long double __nexttowardl(long double __x, long double __y);
double __nextup(double __x);
float __nextupf(float __x);
_Float32 __nextupf32(_Float32 __x);
_Float32x __nextupf32x(_Float32x __x);
_Float64 __nextupf64(_Float64 __x);
_Float64x __nextupf64x(_Float64x __x);
long double __nextupl(long double __x);
int __overflow(FILE *, int);
double __pow(double __x, double __y);
float __powf(float __x, float __y);
_Float32 __powf32(_Float32 __x, _Float32 __y);
_Float32x __powf32x(_Float32x __x, _Float32x __y);
_Float64 __powf64(_Float64 __x, _Float64 __y);
_Float64x __powf64x(_Float64x __x, _Float64x __y);
long double __powl(long double __x, long double __y);
double __pown(double __x, long long __y);
float __pownf(float __x, long long __y);
_Float32 __pownf32(_Float32 __x, long long __y);
_Float32x __pownf32x(_Float32x __x, long long __y);
_Float64 __pownf64(_Float64 __x, long long __y);
_Float64x __pownf64x(_Float64x __x, long long __y);
long double __pownl(long double __x, long long __y);
double __powr(double __x, double __y);
float __powrf(float __x, float __y);
_Float32 __powrf32(_Float32 __x, _Float32 __y);
_Float32x __powrf32x(_Float32x __x, _Float32x __y);
_Float64 __powrf64(_Float64 __x, _Float64 __y);
_Float64x __powrf64x(_Float64x __x, _Float64x __y);
long double __powrl(long double __x, long double __y);
double __remainder(double __x, double __y);
float __remainderf(float __x, float __y);
_Float32 __remainderf32(_Float32 __x, _Float32 __y);
_Float32x __remainderf32x(_Float32x __x, _Float32x __y);
_Float64 __remainderf64(_Float64 __x, _Float64 __y);
_Float64x __remainderf64x(_Float64x __x, _Float64x __y);
long double __remainderl(long double __x, long double __y);
double __remquo(double __x, double __y, int *__quo);
float __remquof(float __x, float __y, int *__quo);
_Float32 __remquof32(_Float32 __x, _Float32 __y, int *__quo);
_Float32x __remquof32x(_Float32x __x, _Float32x __y, int *__quo);
_Float64 __remquof64(_Float64 __x, _Float64 __y, int *__quo);
_Float64x __remquof64x(_Float64x __x, _Float64x __y, int *__quo);
long double __remquol(long double __x, long double __y, int *__quo);
double __rint(double __x);
float __rintf(float __x);
_Float32 __rintf32(_Float32 __x);
_Float32x __rintf32x(_Float32x __x);
_Float64 __rintf64(_Float64 __x);
_Float64x __rintf64x(_Float64x __x);
long double __rintl(long double __x);
double __rootn(double __x, long long __y);
float __rootnf(float __x, long long __y);
_Float32 __rootnf32(_Float32 __x, long long __y);
_Float32x __rootnf32x(_Float32x __x, long long __y);
_Float64 __rootnf64(_Float64 __x, long long __y);
_Float64x __rootnf64x(_Float64x __x, long long __y);
long double __rootnl(long double __x, long long __y);
double __rsqrt(double __x);
float __rsqrtf(float __x);
_Float32 __rsqrtf32(_Float32 __x);
_Float32x __rsqrtf32x(_Float32x __x);
_Float64 __rsqrtf64(_Float64 __x);
_Float64x __rsqrtf64x(_Float64x __x);
long double __rsqrtl(long double __x);
double __scalb(double __x, double __n);
float __scalbf(float __x, float __n);
long double __scalbl(long double __x, long double __n);
double __scalbln(double __x, long __n);
float __scalblnf(float __x, long __n);
_Float32 __scalblnf32(_Float32 __x, long __n);
_Float32x __scalblnf32x(_Float32x __x, long __n);
_Float64 __scalblnf64(_Float64 __x, long __n);
_Float64x __scalblnf64x(_Float64x __x, long __n);
long double __scalblnl(long double __x, long __n);
double __scalbn(double __x, int __n);
float __scalbnf(float __x, int __n);
_Float32 __scalbnf32(_Float32 __x, int __n);
_Float32x __scalbnf32x(_Float32x __x, int __n);
_Float64 __scalbnf64(_Float64 __x, int __n);
_Float64x __scalbnf64x(_Float64x __x, int __n);
long double __scalbnl(long double __x, int __n);
struct cpu_set_t *__sched_cpualloc(size_t __count);
int __sched_cpucount(size_t __setsize, const struct cpu_set_t *__setp);
void __sched_cpufree(struct cpu_set_t *__set);
int __signbit(double __value);
int __signbitf(float __value);
int __signbitl(long double __value);
double __significand(double __x);
float __significandf(float __x);
long double __significandl(long double __x);
int __sigsetjmp(struct __jmp_buf_tag *__env, int __savemask);
double __sin(double __x);
void __sincos(double __x, double *__sinx, double *__cosx);
void __sincosf(float __x, float *__sinx, float *__cosx);
void __sincosf32(_Float32 __x, _Float32 *__sinx, _Float32 *__cosx);
void __sincosf32x(_Float32x __x, _Float32x *__sinx, _Float32x *__cosx);
void __sincosf64(_Float64 __x, _Float64 *__sinx, _Float64 *__cosx);
void __sincosf64x(_Float64x __x, _Float64x *__sinx, _Float64x *__cosx);
void __sincosl(long double __x, long double *__sinx, long double *__cosx);
float __sinf(float __x);
_Float32 __sinf32(_Float32 __x);
_Float32x __sinf32x(_Float32x __x);
_Float64 __sinf64(_Float64 __x);
_Float64x __sinf64x(_Float64x __x);
double __sinh(double __x);
float __sinhf(float __x);
_Float32 __sinhf32(_Float32 __x);
_Float32x __sinhf32x(_Float32x __x);
_Float64 __sinhf64(_Float64 __x);
_Float64x __sinhf64x(_Float64x __x);
long double __sinhl(long double __x);
long double __sinl(long double __x);
double __sinpi(double __x);
float __sinpif(float __x);
_Float32 __sinpif32(_Float32 __x);
_Float32x __sinpif32x(_Float32x __x);
_Float64 __sinpif64(_Float64 __x);
_Float64x __sinpif64x(_Float64x __x);
long double __sinpil(long double __x);
double __sqrt(double __x);
float __sqrtf(float __x);
_Float32 __sqrtf32(_Float32 __x);
_Float32x __sqrtf32x(_Float32x __x);
_Float64 __sqrtf64(_Float64 __x);
_Float64x __sqrtf64x(_Float64x __x);
long double __sqrtl(long double __x);
char *__stpcpy(char *__dest, const char *__src);
char *__stpncpy(char *__dest, const char *__src, size_t __n);
char *__strtok_r(char *__s, const char *__delim, char **__save_ptr);
long __sysconf(int __name);
__sighandler_t __sysv_signal(int __sig, __sighandler_t __handler);
double __tan(double __x);
float __tanf(float __x);
_Float32 __tanf32(_Float32 __x);
_Float32x __tanf32x(_Float32x __x);
_Float64 __tanf64(_Float64 __x);
_Float64x __tanf64x(_Float64x __x);
double __tanh(double __x);
float __tanhf(float __x);
_Float32 __tanhf32(_Float32 __x);
_Float32x __tanhf32x(_Float32x __x);
_Float64 __tanhf64(_Float64 __x);
_Float64x __tanhf64x(_Float64x __x);
long double __tanhl(long double __x);
long double __tanl(long double __x);
double __tanpi(double __x);
float __tanpif(float __x);
_Float32 __tanpif32(_Float32 __x);
_Float32x __tanpif32x(_Float32x __x);
_Float64 __tanpif64(_Float64 __x);
_Float64x __tanpif64x(_Float64x __x);
long double __tanpil(long double __x);
double __tgamma(double);
float __tgammaf(float);
_Float32 __tgammaf32(_Float32);
_Float32x __tgammaf32x(_Float32x);
_Float64 __tgammaf64(_Float64);
_Float64x __tgammaf64x(_Float64x);
long double __tgammal(long double);
int __tolower_l(int __c, locale_t __l);
int __toupper_l(int __c, locale_t __l);
int __uflow(FILE *);
double __ufromfp(double __x, int __round, unsigned int __width);
float __ufromfpf(float __x, int __round, unsigned int __width);
_Float32 __ufromfpf32(_Float32 __x, int __round, unsigned int __width);
_Float32x __ufromfpf32x(_Float32x __x, int __round, unsigned int __width);
_Float64 __ufromfpf64(_Float64 __x, int __round, unsigned int __width);
_Float64x __ufromfpf64x(_Float64x __x, int __round, unsigned int __width);
long double __ufromfpl(long double __x, int __round, unsigned int __width);
double __ufromfpx(double __x, int __round, unsigned int __width);
float __ufromfpxf(float __x, int __round, unsigned int __width);
_Float32 __ufromfpxf32(_Float32 __x, int __round, unsigned int __width);
_Float32x __ufromfpxf32x(_Float32x __x, int __round, unsigned int __width);
_Float64 __ufromfpxf64(_Float64 __x, int __round, unsigned int __width);
_Float64x __ufromfpxf64x(_Float64x __x, int __round, unsigned int __width);
long double __ufromfpxl(long double __x, int __round, unsigned int __width);
__uint16_t __uint16_identity(__uint16_t __x);
__uint32_t __uint32_identity(__uint32_t __x);
__uint64_t __uint64_identity(__uint64_t __x);
char *__xpg_basename(char *__path);
int __xpg_sigpause(int __sig);
double __y0(double);
float __y0f(float);
_Float32 __y0f32(_Float32);
_Float32x __y0f32x(_Float32x);
_Float64 __y0f64(_Float64);
_Float64x __y0f64x(_Float64x);
long double __y0l(long double);
double __y1(double);
float __y1f(float);
_Float32 __y1f32(_Float32);
_Float32x __y1f32x(_Float32x);
_Float64 __y1f64(_Float64);
_Float64x __y1f64x(_Float64x);
long double __y1l(long double);
double __yn(int, double);
float __ynf(int, float);
_Float32 __ynf32(int, _Float32);
_Float32x __ynf32x(int, _Float32x);
_Float64 __ynf64(int, _Float64);
_Float64x __ynf64x(int, _Float64x);
long double __ynl(int, long double);
int _dl_find_object(void *__address, struct dl_find_object *__result);
void _dl_mcount_wrapper_check(void *__selfpc);
_Noreturn void _exit(int __status);
_Noreturn void _longjmp(struct __jmp_buf_tag *__env, int __val);
int _setjmp(struct __jmp_buf_tag *__env);
int _tolower(int);
int _toupper(int);
long a64l(const char *__s);
_Noreturn void abort(void);
int abs(int __x);
int accept(int __fd, union __SOCKADDR_ARG __addr, socklen_t *__addr_len);
int accept4(int __fd, union __SOCKADDR_ARG __addr, socklen_t *__addr_len, int __flags);
int access(const char *__name, int __type);
int acct(const char *__name);
double acos(double __x);
float acosf(float __x);
_Float32 acosf32(_Float32 __x);
_Float32x acosf32x(_Float32x __x);
_Float64 acosf64(_Float64 __x);
_Float64x acosf64x(_Float64x __x);
double acosh(double __x);
float acoshf(float __x);
_Float32 acoshf32(_Float32 __x);
_Float32x acoshf32x(_Float32x __x);
_Float64 acoshf64(_Float64 __x);
_Float64x acoshf64x(_Float64x __x);
long double acoshl(long double __x);
long double acosl(long double __x);
double acospi(double __x);
float acospif(float __x);
_Float32 acospif32(_Float32 __x);
_Float32x acospif32x(_Float32x __x);
_Float64 acospif64(_Float64 __x);
_Float64x acospif64x(_Float64x __x);
long double acospil(long double __x);
int adjtime(const struct timeval *__delta, struct timeval *__olddelta);
int aio_cancel(int __fildes, struct aiocb *__aiocbp);
int aio_cancel64(int __fildes, struct aiocb64 *__aiocbp);
int aio_error(const struct aiocb *__aiocbp);
int aio_error64(const struct aiocb64 *__aiocbp);
int aio_fsync(int __operation, struct aiocb *__aiocbp);
int aio_fsync64(int __operation, struct aiocb64 *__aiocbp);
void aio_init(const struct aioinit *__init);
int aio_read(struct aiocb *__aiocbp);
int aio_read64(struct aiocb64 *__aiocbp);
__ssize_t aio_return(struct aiocb *__aiocbp);
__ssize_t aio_return64(struct aiocb64 *__aiocbp);
int aio_suspend(const struct aiocb *const *__list, int __nent, const struct timespec *__timeout);
int aio_suspend64(const struct aiocb64 *const *__list, int __nent, const struct timespec *__timeout);
int aio_write(struct aiocb *__aiocbp);
int aio_write64(struct aiocb64 *__aiocbp);
unsigned int alarm(unsigned int __seconds);
void *aligned_alloc(size_t __alignment, size_t __size);
void *alloca(size_t __size);
int alphasort(const struct dirent **__e1, const struct dirent **__e2);
int alphasort64(const struct dirent64 **__e1, const struct dirent64 **__e2);
__uint32_t arc4random(void);
void arc4random_buf(void *__buf, size_t __size);
__uint32_t arc4random_uniform(__uint32_t __upper_bound);
char *asctime(const struct tm *__tp);
char *asctime_r(const struct tm *__tp, char *__buf);
double asin(double __x);
float asinf(float __x);
_Float32 asinf32(_Float32 __x);
_Float32x asinf32x(_Float32x __x);
_Float64 asinf64(_Float64 __x);
_Float64x asinf64x(_Float64x __x);
double asinh(double __x);
float asinhf(float __x);
_Float32 asinhf32(_Float32 __x);
_Float32x asinhf32x(_Float32x __x);
_Float64 asinhf64(_Float64 __x);
_Float64x asinhf64x(_Float64x __x);
long double asinhl(long double __x);
long double asinl(long double __x);
double asinpi(double __x);
float asinpif(float __x);
_Float32 asinpif32(_Float32 __x);
_Float32x asinpif32x(_Float32x __x);
_Float64 asinpif64(_Float64 __x);
_Float64x asinpif64x(_Float64x __x);
long double asinpil(long double __x);
int asprintf(char **__ptr, const char *__fmt, ...);
int at_quick_exit(void (*__func)(void));
double atan(double __x);
double atan2(double __y, double __x);
float atan2f(float __y, float __x);
_Float32 atan2f32(_Float32 __y, _Float32 __x);
_Float32x atan2f32x(_Float32x __y, _Float32x __x);
_Float64 atan2f64(_Float64 __y, _Float64 __x);
_Float64x atan2f64x(_Float64x __y, _Float64x __x);
long double atan2l(long double __y, long double __x);
double atan2pi(double __y, double __x);
float atan2pif(float __y, float __x);
_Float32 atan2pif32(_Float32 __y, _Float32 __x);
_Float32x atan2pif32x(_Float32x __y, _Float32x __x);
_Float64 atan2pif64(_Float64 __y, _Float64 __x);
_Float64x atan2pif64x(_Float64x __y, _Float64x __x);
long double atan2pil(long double __y, long double __x);
float atanf(float __x);
_Float32 atanf32(_Float32 __x);
_Float32x atanf32x(_Float32x __x);
_Float64 atanf64(_Float64 __x);
_Float64x atanf64x(_Float64x __x);
double atanh(double __x);
float atanhf(float __x);
_Float32 atanhf32(_Float32 __x);
_Float32x atanhf32x(_Float32x __x);
_Float64 atanhf64(_Float64 __x);
_Float64x atanhf64x(_Float64x __x);
long double atanhl(long double __x);
long double atanl(long double __x);
double atanpi(double __x);
float atanpif(float __x);
_Float32 atanpif32(_Float32 __x);
_Float32x atanpif32x(_Float32x __x);
_Float64 atanpif64(_Float64 __x);
_Float64x atanpif64x(_Float64x __x);
long double atanpil(long double __x);
int atexit(void (*__func)(void));
double atof(const char *__nptr);
int atoi(const char *__nptr);
long atol(const char *__nptr);
long long atoll(const char *__nptr);
int backtrace(void **__array, int __size);
char **backtrace_symbols(void *const *__array, int __size);
void backtrace_symbols_fd(void *const *__array, int __size, int __fd);
char *basename(const char *__filename);
int bcmp(const void *__s1, const void *__s2, size_t __n);
void bcopy(const void *__src, void *__dest, size_t __n);
int bind(int __fd, union __CONST_SOCKADDR_ARG __addr, socklen_t __len);
int bindresvport(int __sockfd, struct sockaddr_in *__sock_in);
int bindresvport6(int __sockfd, struct sockaddr_in6 *__sock_in);
int brk(void *__addr);
void *bsearch(const void *__key, const void *__base, size_t __nmemb, size_t __size, __compar_fn_t __compar);
wint_t btowc(int __c);
void bzero(void *__s, size_t __n);
void call_once(once_flag *__flag, void (*__func)(void));
void *calloc(size_t __nmemb, size_t __size);
int canonicalize(double *__cx, const double *__x);
char *canonicalize_file_name(const char *__name);
int canonicalizef(float *__cx, const float *__x);
int canonicalizef32(_Float32 *__cx, const _Float32 *__x);
int canonicalizef32x(_Float32x *__cx, const _Float32x *__x);
int canonicalizef64(_Float64 *__cx, const _Float64 *__x);
int canonicalizef64x(_Float64x *__cx, const _Float64x *__x);
int canonicalizel(long double *__cx, const long double *__x);
int catclose(nl_catd __catalog);
char *catgets(nl_catd __catalog, int __set, int __number, const char *__string);
nl_catd catopen(const char *__cat_name, int __flag);
double cbrt(double __x);
float cbrtf(float __x);
_Float32 cbrtf32(_Float32 __x);
_Float32x cbrtf32x(_Float32x __x);
_Float64 cbrtf64(_Float64 __x);
_Float64x cbrtf64x(_Float64x __x);
long double cbrtl(long double __x);
double ceil(double __x);
float ceilf(float __x);
_Float32 ceilf32(_Float32 __x);
_Float32x ceilf32x(_Float32x __x);
_Float64 ceilf64(_Float64 __x);
_Float64x ceilf64x(_Float64x __x);
long double ceill(long double __x);
baud_t cfgetibaud(const struct termios *__termios_p);
speed_t cfgetispeed(const struct termios *__termios_p);
baud_t cfgetobaud(const struct termios *__termios_p);
speed_t cfgetospeed(const struct termios *__termios_p);
void cfmakeraw(struct termios *__termios_p);
int cfsetbaud(struct termios *__termios_p, baud_t __baud);
int cfsetibaud(struct termios *__termios_p, baud_t __baud);
int cfsetispeed(struct termios *__termios_p, speed_t __speed);
int cfsetobaud(struct termios *__termios_p, baud_t __baud);
int cfsetospeed(struct termios *__termios_p, speed_t __speed);
int cfsetspeed(struct termios *__termios_p, speed_t __speed);
int chdir(const char *__path);
int chmod(const char *__file, __mode_t __mode);
int chown(const char *__file, __uid_t __owner, __gid_t __group);
int chroot(const char *__path);
int clearenv(void);
void clearerr(FILE *__stream);
void clearerr_unlocked(FILE *__stream);
clock_t clock(void);
int clock_adjtime(__clockid_t __clock_id, struct timex *__utx);
int clock_getcpuclockid(pid_t __pid, clockid_t *__clock_id);
int clock_getres(clockid_t __clock_id, struct timespec *__res);
int clock_gettime(clockid_t __clock_id, struct timespec *__tp);
int clock_nanosleep(clockid_t __clock_id, int __flags, const struct timespec *__req, struct timespec *__rem);
int clock_settime(clockid_t __clock_id, const struct timespec *__tp);
int clone(int (*__fn)(void *__arg), void *__child_stack, int __flags, void *__arg, ...);
int close(int __fd);
int close_range(unsigned int __fd, unsigned int __max_fd, int __flags);
int closedir(DIR *__dirp);
void closefrom(int __lowfd);
void closelog(void);
double compoundn(double __x, long long __y);
float compoundnf(float __x, long long __y);
_Float32 compoundnf32(_Float32 __x, long long __y);
_Float32x compoundnf32x(_Float32x __x, long long __y);
_Float64 compoundnf64(_Float64 __x, long long __y);
_Float64x compoundnf64x(_Float64x __x, long long __y);
long double compoundnl(long double __x, long long __y);
size_t confstr(int __name, char *__buf, size_t __len);
int connect(int __fd, union __CONST_SOCKADDR_ARG __addr, socklen_t __len);
ssize_t copy_file_range(int __infd, __off64_t *__pinoff, int __outfd, __off64_t *__poutoff, size_t __length, unsigned int __flags);
double copysign(double __x, double __y);
float copysignf(float __x, float __y);
_Float32 copysignf32(_Float32 __x, _Float32 __y);
_Float32x copysignf32x(_Float32x __x, _Float32x __y);
_Float64 copysignf64(_Float64 __x, _Float64 __y);
_Float64x copysignf64x(_Float64x __x, _Float64x __y);
long double copysignl(long double __x, long double __y);
double cos(double __x);
float cosf(float __x);
_Float32 cosf32(_Float32 __x);
_Float32x cosf32x(_Float32x __x);
_Float64 cosf64(_Float64 __x);
_Float64x cosf64x(_Float64x __x);
double cosh(double __x);
float coshf(float __x);
_Float32 coshf32(_Float32 __x);
_Float32x coshf32x(_Float32x __x);
_Float64 coshf64(_Float64 __x);
_Float64x coshf64x(_Float64x __x);
long double coshl(long double __x);
long double cosl(long double __x);
double cospi(double __x);
float cospif(float __x);
_Float32 cospif32(_Float32 __x);
_Float32x cospif32x(_Float32x __x);
_Float64 cospif64(_Float64 __x);
_Float64x cospif64x(_Float64x __x);
long double cospil(long double __x);
int creat(const char *__file, mode_t __mode);
int creat64(const char *__file, mode_t __mode);
char *crypt(const char *__key, const char *__salt);
char *ctermid(char *__s);
char *ctime(const time_t *__timer);
char *ctime_r(const time_t *__timer, char *__buf);
char *cuserid(char *__s);
double daddl(long double __x, long double __y);
int daemon(int __nochdir, int __noclose);
double ddivl(long double __x, long double __y);
double dfmal(long double __x, long double __y, long double __z);
double difftime(time_t __time1, time_t __time0);
int dirfd(DIR *__dirp);
char *dirname(char *__path);
struct div_t div(int __numer, int __denom);
int dl_iterate_phdr(int (*__callback)(struct dl_phdr_info *, size_t, void *), void *__data);
int dladdr(const void *__address, struct Dl_info *__info);
int dladdr1(const void *__address, struct Dl_info *__info, void **__extra_info, int __flags);
int dlclose(void *__handle);
char *dlerror(void);
int dlinfo(void *__handle, int __request, void *__arg);
void *dlmopen(Lmid_t __nsid, const char *__file, int __mode);
void *dlopen(const char *__file, int __mode);
void *dlsym(void *__handle, const char *__name);
void *dlvsym(void *__handle, const char *__name, const char *__version);
double dmull(long double __x, long double __y);
int dprintf(int __fd, const char *__fmt, ...);
double drand48(void);
int drand48_r(struct drand48_data *__buffer, double *__result);
double drem(double __x, double __y);
float dremf(float __x, float __y);
long double dreml(long double __x, long double __y);
double dsqrtl(long double __x);
double dsubl(long double __x, long double __y);
int dup(int __fd);
int dup2(int __fd, int __fd2);
int dup3(int __fd, int __fd2, int __flags);
locale_t duplocale(locale_t __dataset);
int dysize(int __year);
int eaccess(const char *__name, int __type);
char *ecvt(double __value, int __ndigit, int *__decpt, int *__sign);
int ecvt_r(double __value, int __ndigit, int *__decpt, int *__sign, char *__buf, size_t __len);
void endgrent(void);
void endhostent(void);
void endnetent(void);
void endnetgrent(void);
void endprotoent(void);
void endpwent(void);
void endrpcent(void);
void endservent(void);
void endspent(void);
void endusershell(void);
void endutent(void);
int epoll_create(int __size);
int epoll_create1(int __flags);
int epoll_ctl(int __epfd, int __op, int __fd, struct epoll_event *__event);
int epoll_pwait(int __epfd, struct epoll_event *__events, int __maxevents, int __timeout, const struct __sigset_t *__ss);
int epoll_pwait2(int __epfd, struct epoll_event *__events, int __maxevents, const struct timespec *__timeout, const struct __sigset_t *__ss);
int epoll_wait(int __epfd, struct epoll_event *__events, int __maxevents, int __timeout);
double erand48(unsigned short *__xsubi);
int erand48_r(unsigned short *__xsubi, struct drand48_data *__buffer, double *__result);
double erf(double);
double erfc(double);
float erfcf(float);
_Float32 erfcf32(_Float32);
_Float32x erfcf32x(_Float32x);
_Float64 erfcf64(_Float64);
_Float64x erfcf64x(_Float64x);
long double erfcl(long double);
float erff(float);
_Float32 erff32(_Float32);
_Float32x erff32x(_Float32x);
_Float64 erff64(_Float64);
_Float64x erff64x(_Float64x);
long double erfl(long double);
_Noreturn void err(int __status, const char *__format, ...);
void error(int __status, int __errnum, const char *__format, ...);
void error_at_line(int __status, int __errnum, const char *__fname, unsigned int __lineno, const char *__format, ...);
_Noreturn void errx(int __status, const char *__format, ...);
int euidaccess(const char *__name, int __type);
int eventfd(unsigned int __count, int __flags);
int eventfd_read(int __fd, eventfd_t *__value);
int eventfd_write(int __fd, eventfd_t __value);
int execl(const char *__path, const char *__arg, ...);
int execle(const char *__path, const char *__arg, ...);
int execlp(const char *__file, const char *__arg, ...);
int execv(const char *__path, char *const *__argv);
int execve(const char *__path, char *const *__argv, char *const *__envp);
int execveat(int __fd, const char *__path, char *const *__argv, char *const *__envp, int __flags);
int execvp(const char *__file, char *const *__argv);
int execvpe(const char *__file, char *const *__argv, char *const *__envp);
_Noreturn void exit(int __status);
double exp(double __x);
double exp10(double __x);
float exp10f(float __x);
_Float32 exp10f32(_Float32 __x);
_Float32x exp10f32x(_Float32x __x);
_Float64 exp10f64(_Float64 __x);
_Float64x exp10f64x(_Float64x __x);
long double exp10l(long double __x);
double exp10m1(double __x);
float exp10m1f(float __x);
_Float32 exp10m1f32(_Float32 __x);
_Float32x exp10m1f32x(_Float32x __x);
_Float64 exp10m1f64(_Float64 __x);
_Float64x exp10m1f64x(_Float64x __x);
long double exp10m1l(long double __x);
double exp2(double __x);
float exp2f(float __x);
_Float32 exp2f32(_Float32 __x);
_Float32x exp2f32x(_Float32x __x);
_Float64 exp2f64(_Float64 __x);
_Float64x exp2f64x(_Float64x __x);
long double exp2l(long double __x);
double exp2m1(double __x);
float exp2m1f(float __x);
_Float32 exp2m1f32(_Float32 __x);
_Float32x exp2m1f32x(_Float32x __x);
_Float64 exp2m1f64(_Float64 __x);
_Float64x exp2m1f64x(_Float64x __x);
long double exp2m1l(long double __x);
float expf(float __x);
_Float32 expf32(_Float32 __x);
_Float32x expf32x(_Float32x __x);
_Float64 expf64(_Float64 __x);
_Float64x expf64x(_Float64x __x);
long double expl(long double __x);
void explicit_bzero(void *__s, size_t __n);
double expm1(double __x);
float expm1f(float __x);
_Float32 expm1f32(_Float32 __x);
_Float32x expm1f32x(_Float32x __x);
_Float64 expm1f64(_Float64 __x);
_Float64x expm1f64x(_Float64x __x);
long double expm1l(long double __x);
_Float32 f32addf32x(_Float32x __x, _Float32x __y);
_Float32 f32addf64(_Float64 __x, _Float64 __y);
_Float32 f32addf64x(_Float64x __x, _Float64x __y);
_Float32 f32divf32x(_Float32x __x, _Float32x __y);
_Float32 f32divf64(_Float64 __x, _Float64 __y);
_Float32 f32divf64x(_Float64x __x, _Float64x __y);
_Float32 f32fmaf32x(_Float32x __x, _Float32x __y, _Float32x __z);
_Float32 f32fmaf64(_Float64 __x, _Float64 __y, _Float64 __z);
_Float32 f32fmaf64x(_Float64x __x, _Float64x __y, _Float64x __z);
_Float32 f32mulf32x(_Float32x __x, _Float32x __y);
_Float32 f32mulf64(_Float64 __x, _Float64 __y);
_Float32 f32mulf64x(_Float64x __x, _Float64x __y);
_Float32 f32sqrtf32x(_Float32x __x);
_Float32 f32sqrtf64(_Float64 __x);
_Float32 f32sqrtf64x(_Float64x __x);
_Float32 f32subf32x(_Float32x __x, _Float32x __y);
_Float32 f32subf64(_Float64 __x, _Float64 __y);
_Float32 f32subf64x(_Float64x __x, _Float64x __y);
_Float32x f32xaddf64(_Float64 __x, _Float64 __y);
_Float32x f32xaddf64x(_Float64x __x, _Float64x __y);
_Float32x f32xdivf64(_Float64 __x, _Float64 __y);
_Float32x f32xdivf64x(_Float64x __x, _Float64x __y);
_Float32x f32xfmaf64(_Float64 __x, _Float64 __y, _Float64 __z);
_Float32x f32xfmaf64x(_Float64x __x, _Float64x __y, _Float64x __z);
_Float32x f32xmulf64(_Float64 __x, _Float64 __y);
_Float32x f32xmulf64x(_Float64x __x, _Float64x __y);
_Float32x f32xsqrtf64(_Float64 __x);
_Float32x f32xsqrtf64x(_Float64x __x);
_Float32x f32xsubf64(_Float64 __x, _Float64 __y);
_Float32x f32xsubf64x(_Float64x __x, _Float64x __y);
_Float64 f64addf64x(_Float64x __x, _Float64x __y);
_Float64 f64divf64x(_Float64x __x, _Float64x __y);
_Float64 f64fmaf64x(_Float64x __x, _Float64x __y, _Float64x __z);
_Float64 f64mulf64x(_Float64x __x, _Float64x __y);
_Float64 f64sqrtf64x(_Float64x __x);
_Float64 f64subf64x(_Float64x __x, _Float64x __y);
double fabs(double __x);
float fabsf(float __x);
_Float32 fabsf32(_Float32 __x);
_Float32x fabsf32x(_Float32x __x);
_Float64 fabsf64(_Float64 __x);
_Float64x fabsf64x(_Float64x __x);
long double fabsl(long double __x);
int faccessat(int __fd, const char *__file, int __type, int __flag);
float fadd(double __x, double __y);
float faddl(long double __x, long double __y);
int fallocate(int __fd, int __mode, __off_t __offset, __off_t __len);
int fallocate64(int __fd, int __mode, __off64_t __offset, __off64_t __len);
int fchdir(int __fd);
int fchmod(int __fd, __mode_t __mode);
int fchmodat(int __fd, const char *__file, __mode_t __mode, int __flag);
int fchown(int __fd, __uid_t __owner, __gid_t __group);
int fchownat(int __fd, const char *__file, __uid_t __owner, __gid_t __group, int __flag);
int fclose(FILE *__stream);
int fcloseall(void);
int fcntl(int __fd, int __cmd, ...);
int fcntl64(int __fd, int __cmd, ...);
char *fcvt(double __value, int __ndigit, int *__decpt, int *__sign);
int fcvt_r(double __value, int __ndigit, int *__decpt, int *__sign, char *__buf, size_t __len);
int fdatasync(int __fildes);
double fdim(double __x, double __y);
float fdimf(float __x, float __y);
_Float32 fdimf32(_Float32 __x, _Float32 __y);
_Float32x fdimf32x(_Float32x __x, _Float32x __y);
_Float64 fdimf64(_Float64 __x, _Float64 __y);
_Float64x fdimf64x(_Float64x __x, _Float64x __y);
long double fdiml(long double __x, long double __y);
float fdiv(double __x, double __y);
float fdivl(long double __x, long double __y);
FILE *fdopen(int __fd, const char *__modes);
DIR *fdopendir(int __fd);
int feclearexcept(int __excepts);
int fedisableexcept(int __excepts);
int feenableexcept(int __excepts);
int fegetenv(struct fenv_t *__envp);
int fegetexcept(void);
int fegetexceptflag(fexcept_t *__flagp, int __excepts);
int fegetmode(struct femode_t *__modep);
int fegetround(void);
int feholdexcept(struct fenv_t *__envp);
int feof(FILE *__stream);
int feof_unlocked(FILE *__stream);
int feraiseexcept(int __excepts);
int ferror(FILE *__stream);
int ferror_unlocked(FILE *__stream);
int fesetenv(const struct fenv_t *__envp);
int fesetexcept(int __excepts);
int fesetexceptflag(const fexcept_t *__flagp, int __excepts);
int fesetmode(const struct femode_t *__modep);
int fesetround(int __rounding_direction);
int fetestexcept(int __excepts);
int fetestexceptflag(const fexcept_t *__flagp, int __excepts);
int feupdateenv(const struct fenv_t *__envp);
int fexecve(int __fd, char *const *__argv, char *const *__envp);
int fflush(FILE *__stream);
int fflush_unlocked(FILE *__stream);
float ffma(double __x, double __y, double __z);
float ffmal(long double __x, long double __y, long double __z);
int ffs(int __i);
int ffsl(long __l);
int ffsll(long long __ll);
int fgetc(FILE *__stream);
int fgetc_unlocked(FILE *__stream);
struct group *fgetgrent(FILE *__stream);
int fgetgrent_r(FILE *__stream, struct group *__resultbuf, char *__buffer, size_t __buflen, struct group **__result);
int fgetpos(FILE *__stream, fpos_t *__pos);
int fgetpos64(FILE *__stream, fpos64_t *__pos);
struct passwd *fgetpwent(FILE *__stream);
int fgetpwent_r(FILE *__stream, struct passwd *__resultbuf, char *__buffer, size_t __buflen, struct passwd **__result);
char *fgets(char *__s, int __n, FILE *__stream);
char *fgets_unlocked(char *__s, int __n, FILE *__stream);
struct spwd *fgetspent(FILE *__stream);
int fgetspent_r(FILE *__stream, struct spwd *__result_buf, char *__buffer, size_t __buflen, struct spwd **__result);
wint_t fgetwc(__FILE *__stream);
wint_t fgetwc_unlocked(__FILE *__stream);
int *fgetws(int *__ws, int __n, __FILE *__stream);
int *fgetws_unlocked(int *__ws, int __n, __FILE *__stream);
ssize_t fgetxattr(int __fd, const char *__name, void *__value, size_t __size);
int fileno(FILE *__stream);
int fileno_unlocked(FILE *__stream);
int finite(double __value);
int finitef(float __value);
int finitel(long double __value);
ssize_t flistxattr(int __fd, char *__list, size_t __size);
int flock(int __fd, int __operation);
void flockfile(FILE *__stream);
double floor(double __x);
float floorf(float __x);
_Float32 floorf32(_Float32 __x);
_Float32x floorf32x(_Float32x __x);
_Float64 floorf64(_Float64 __x);
_Float64x floorf64x(_Float64x __x);
long double floorl(long double __x);
double fma(double __x, double __y, double __z);
float fmaf(float __x, float __y, float __z);
_Float32 fmaf32(_Float32 __x, _Float32 __y, _Float32 __z);
_Float32x fmaf32x(_Float32x __x, _Float32x __y, _Float32x __z);
_Float64 fmaf64(_Float64 __x, _Float64 __y, _Float64 __z);
_Float64x fmaf64x(_Float64x __x, _Float64x __y, _Float64x __z);
long double fmal(long double __x, long double __y, long double __z);
double fmax(double __x, double __y);
float fmaxf(float __x, float __y);
_Float32 fmaxf32(_Float32 __x, _Float32 __y);
_Float32x fmaxf32x(_Float32x __x, _Float32x __y);
_Float64 fmaxf64(_Float64 __x, _Float64 __y);
_Float64x fmaxf64x(_Float64x __x, _Float64x __y);
double fmaximum(double __x, double __y);
double fmaximum_mag(double __x, double __y);
double fmaximum_mag_num(double __x, double __y);
float fmaximum_mag_numf(float __x, float __y);
_Float32 fmaximum_mag_numf32(_Float32 __x, _Float32 __y);
_Float32x fmaximum_mag_numf32x(_Float32x __x, _Float32x __y);
_Float64 fmaximum_mag_numf64(_Float64 __x, _Float64 __y);
_Float64x fmaximum_mag_numf64x(_Float64x __x, _Float64x __y);
long double fmaximum_mag_numl(long double __x, long double __y);
float fmaximum_magf(float __x, float __y);
_Float32 fmaximum_magf32(_Float32 __x, _Float32 __y);
_Float32x fmaximum_magf32x(_Float32x __x, _Float32x __y);
_Float64 fmaximum_magf64(_Float64 __x, _Float64 __y);
_Float64x fmaximum_magf64x(_Float64x __x, _Float64x __y);
long double fmaximum_magl(long double __x, long double __y);
double fmaximum_num(double __x, double __y);
float fmaximum_numf(float __x, float __y);
_Float32 fmaximum_numf32(_Float32 __x, _Float32 __y);
_Float32x fmaximum_numf32x(_Float32x __x, _Float32x __y);
_Float64 fmaximum_numf64(_Float64 __x, _Float64 __y);
_Float64x fmaximum_numf64x(_Float64x __x, _Float64x __y);
long double fmaximum_numl(long double __x, long double __y);
float fmaximumf(float __x, float __y);
_Float32 fmaximumf32(_Float32 __x, _Float32 __y);
_Float32x fmaximumf32x(_Float32x __x, _Float32x __y);
_Float64 fmaximumf64(_Float64 __x, _Float64 __y);
_Float64x fmaximumf64x(_Float64x __x, _Float64x __y);
long double fmaximuml(long double __x, long double __y);
long double fmaxl(long double __x, long double __y);
double fmaxmag(double __x, double __y);
float fmaxmagf(float __x, float __y);
_Float32 fmaxmagf32(_Float32 __x, _Float32 __y);
_Float32x fmaxmagf32x(_Float32x __x, _Float32x __y);
_Float64 fmaxmagf64(_Float64 __x, _Float64 __y);
_Float64x fmaxmagf64x(_Float64x __x, _Float64x __y);
long double fmaxmagl(long double __x, long double __y);
FILE *fmemopen(void *__s, size_t __len, const char *__modes);
double fmin(double __x, double __y);
float fminf(float __x, float __y);
_Float32 fminf32(_Float32 __x, _Float32 __y);
_Float32x fminf32x(_Float32x __x, _Float32x __y);
_Float64 fminf64(_Float64 __x, _Float64 __y);
_Float64x fminf64x(_Float64x __x, _Float64x __y);
double fminimum(double __x, double __y);
double fminimum_mag(double __x, double __y);
double fminimum_mag_num(double __x, double __y);
float fminimum_mag_numf(float __x, float __y);
_Float32 fminimum_mag_numf32(_Float32 __x, _Float32 __y);
_Float32x fminimum_mag_numf32x(_Float32x __x, _Float32x __y);
_Float64 fminimum_mag_numf64(_Float64 __x, _Float64 __y);
_Float64x fminimum_mag_numf64x(_Float64x __x, _Float64x __y);
long double fminimum_mag_numl(long double __x, long double __y);
float fminimum_magf(float __x, float __y);
_Float32 fminimum_magf32(_Float32 __x, _Float32 __y);
_Float32x fminimum_magf32x(_Float32x __x, _Float32x __y);
_Float64 fminimum_magf64(_Float64 __x, _Float64 __y);
_Float64x fminimum_magf64x(_Float64x __x, _Float64x __y);
long double fminimum_magl(long double __x, long double __y);
double fminimum_num(double __x, double __y);
float fminimum_numf(float __x, float __y);
_Float32 fminimum_numf32(_Float32 __x, _Float32 __y);
_Float32x fminimum_numf32x(_Float32x __x, _Float32x __y);
_Float64 fminimum_numf64(_Float64 __x, _Float64 __y);
_Float64x fminimum_numf64x(_Float64x __x, _Float64x __y);
long double fminimum_numl(long double __x, long double __y);
float fminimumf(float __x, float __y);
_Float32 fminimumf32(_Float32 __x, _Float32 __y);
_Float32x fminimumf32x(_Float32x __x, _Float32x __y);
_Float64 fminimumf64(_Float64 __x, _Float64 __y);
_Float64x fminimumf64x(_Float64x __x, _Float64x __y);
long double fminimuml(long double __x, long double __y);
long double fminl(long double __x, long double __y);
double fminmag(double __x, double __y);
float fminmagf(float __x, float __y);
_Float32 fminmagf32(_Float32 __x, _Float32 __y);
_Float32x fminmagf32x(_Float32x __x, _Float32x __y);
_Float64 fminmagf64(_Float64 __x, _Float64 __y);
_Float64x fminmagf64x(_Float64x __x, _Float64x __y);
long double fminmagl(long double __x, long double __y);
double fmod(double __x, double __y);
float fmodf(float __x, float __y);
_Float32 fmodf32(_Float32 __x, _Float32 __y);
_Float32x fmodf32x(_Float32x __x, _Float32x __y);
_Float64 fmodf64(_Float64 __x, _Float64 __y);
_Float64x fmodf64x(_Float64x __x, _Float64x __y);
long double fmodl(long double __x, long double __y);
float fmul(double __x, double __y);
float fmull(long double __x, long double __y);
int fnmatch(const char *__pattern, const char *__name, int __flags);
FILE *fopen(const char *__filename, const char *__modes);
FILE *fopen64(const char *__filename, const char *__modes);
FILE *fopencookie(void *__magic_cookie, const char *__modes, cookie_io_functions_t __io_funcs);
__pid_t fork(void);
long fpathconf(int __fd, int __name);
int fprintf(FILE *__stream, const char *__format, ...);
int fputc(int __c, FILE *__stream);
int fputc_unlocked(int __c, FILE *__stream);
int fputs(const char *__s, FILE *__stream);
int fputs_unlocked(const char *__s, FILE *__stream);
wint_t fputwc(int __wc, __FILE *__stream);
wint_t fputwc_unlocked(int __wc, __FILE *__stream);
int fputws(const int *__ws, __FILE *__stream);
int fputws_unlocked(const int *__ws, __FILE *__stream);
size_t fread(void *__ptr, size_t __size, size_t __n, FILE *__stream);
size_t fread_unlocked(void *__ptr, size_t __size, size_t __n, FILE *__stream);
void free(void *__ptr);
void free_aligned_sized(void *__ptr, size_t __alignment, size_t __size);
void free_sized(void *__ptr, size_t __size);
void freeaddrinfo(struct addrinfo *__ai);
void freeifaddrs(struct ifaddrs *__ifa);
void freelocale(locale_t __dataset);
int fremovexattr(int __fd, const char *__name);
FILE *freopen(const char *__filename, const char *__modes, FILE *__stream);
FILE *freopen64(const char *__filename, const char *__modes, FILE *__stream);
double frexp(double __x, int *__exponent);
float frexpf(float __x, int *__exponent);
_Float32 frexpf32(_Float32 __x, int *__exponent);
_Float32x frexpf32x(_Float32x __x, int *__exponent);
_Float64 frexpf64(_Float64 __x, int *__exponent);
_Float64x frexpf64x(_Float64x __x, int *__exponent);
long double frexpl(long double __x, int *__exponent);
double fromfp(double __x, int __round, unsigned int __width);
float fromfpf(float __x, int __round, unsigned int __width);
_Float32 fromfpf32(_Float32 __x, int __round, unsigned int __width);
_Float32x fromfpf32x(_Float32x __x, int __round, unsigned int __width);
_Float64 fromfpf64(_Float64 __x, int __round, unsigned int __width);
_Float64x fromfpf64x(_Float64x __x, int __round, unsigned int __width);
long double fromfpl(long double __x, int __round, unsigned int __width);
double fromfpx(double __x, int __round, unsigned int __width);
float fromfpxf(float __x, int __round, unsigned int __width);
_Float32 fromfpxf32(_Float32 __x, int __round, unsigned int __width);
_Float32x fromfpxf32x(_Float32x __x, int __round, unsigned int __width);
_Float64 fromfpxf64(_Float64 __x, int __round, unsigned int __width);
_Float64x fromfpxf64x(_Float64x __x, int __round, unsigned int __width);
long double fromfpxl(long double __x, int __round, unsigned int __width);
int fscanf(FILE *__stream, const char *__format, ...);
int fseek(FILE *__stream, long __off, int __whence);
int fseeko(FILE *__stream, __off_t __off, int __whence);
int fseeko64(FILE *__stream, __off64_t __off, int __whence);
int fsetpos(FILE *__stream, const fpos_t *__pos);
int fsetpos64(FILE *__stream, const fpos64_t *__pos);
int fsetxattr(int __fd, const char *__name, const void *__value, size_t __size, int __flags);
float fsqrt(double __x);
float fsqrtl(long double __x);
int fstat(int __fd, struct stat *__buf);
int fstat64(int __fd, struct stat64 *__buf);
int fstatat(int __fd, const char *__file, struct stat *__buf, int __flag);
int fstatat64(int __fd, const char *__file, struct stat64 *__buf, int __flag);
int fstatvfs(int __fildes, struct statvfs *__buf);
int fstatvfs64(int __fildes, struct statvfs64 *__buf);
float fsub(double __x, double __y);
float fsubl(long double __x, long double __y);
int fsync(int __fd);
long ftell(FILE *__stream);
__off_t ftello(FILE *__stream);
__off64_t ftello64(FILE *__stream);
key_t ftok(const char *__pathname, int __proj_id);
int ftruncate(int __fd, __off_t __length);
int ftruncate64(int __fd, __off64_t __length);
int ftrylockfile(FILE *__stream);
FTSENT64 *fts64_children(struct FTS64 *, int);
int fts64_close(struct FTS64 *);
struct FTS64 *fts64_open(char *const *, int, int (*)(const FTSENT64 **, const FTSENT64 **));
FTSENT64 *fts64_read(struct FTS64 *);
int fts64_set(struct FTS64 *, FTSENT64 *, int);
FTSENT *fts_children(struct FTS *, int);
int fts_close(struct FTS *);
struct FTS *fts_open(char *const *, int, int (*)(const FTSENT **, const FTSENT **));
FTSENT *fts_read(struct FTS *);
int fts_set(struct FTS *, FTSENT *, int);
int ftw(const char *__dir, __ftw_func_t __func, int __descriptors);
int ftw64(const char *__dir, __ftw64_func_t __func, int __descriptors);
void funlockfile(FILE *__stream);
int futimens(int __fd, const struct timespec *__times);
int futimes(int __fd, const struct timeval *__tvp);
int futimesat(int __fd, const char *__file, const struct timeval *__tvp);
int fwide(__FILE *__fp, int __mode);
int fwprintf(__FILE *__stream, const int *__format, ...);
size_t fwrite(const void *__ptr, size_t __size, size_t __n, FILE *__s);
size_t fwrite_unlocked(const void *__ptr, size_t __size, size_t __n, FILE *__stream);
int fwscanf(__FILE *__stream, const int *__format, ...);
int gai_cancel(struct gaicb *__gaicbp);
int gai_error(struct gaicb *__req);
const char *gai_strerror(int __ecode);
int gai_suspend(const struct gaicb *const *__list, int __ent, const struct timespec *__timeout);
double gamma(double);
float gammaf(float);
long double gammal(long double);
char *gcvt(double __value, int __ndigit, char *__buf);
long get_avphys_pages(void);
char *get_current_dir_name(void);
int get_nprocs(void);
int get_nprocs_conf(void);
long get_phys_pages(void);
int getaddrinfo(const char *__name, const char *__service, const struct addrinfo *__req, struct addrinfo **__pai);
int getaddrinfo_a(int __mode, struct gaicb **__list, int __ent, struct sigevent *__sig);
unsigned long getauxval(unsigned long __type);
int getc(FILE *__stream);
int getc_unlocked(FILE *__stream);
int getchar(void);
int getchar_unlocked(void);
int getcontext(struct ucontext_t *__ucp);
int getcpu(unsigned int *, unsigned int *);
char *getcwd(char *__buf, size_t __size);
struct tm *getdate(const char *__string);
int getdate_r(const char *__string, struct tm *__resbufp);
__ssize_t getdelim(char **__lineptr, size_t *__n, int __delimiter, FILE *__stream);
__ssize_t getdents64(int __fd, void *__buffer, size_t __length);
__ssize_t getdirentries(int __fd, char *__buf, size_t __nbytes, __off_t *__basep);
__ssize_t getdirentries64(int __fd, char *__buf, size_t __nbytes, __off64_t *__basep);
int getdomainname(char *__name, size_t __len);
int getdtablesize(void);
__gid_t getegid(void);
int getentropy(void *__buffer, size_t __length);
char *getenv(const char *__name);
__uid_t geteuid(void);
__gid_t getgid(void);
struct group *getgrent(void);
int getgrent_r(struct group *__resultbuf, char *__buffer, size_t __buflen, struct group **__result);
struct group *getgrgid(__gid_t __gid);
int getgrgid_r(__gid_t __gid, struct group *__resultbuf, char *__buffer, size_t __buflen, struct group **__result);
struct group *getgrnam(const char *__name);
int getgrnam_r(const char *__name, struct group *__resultbuf, char *__buffer, size_t __buflen, struct group **__result);
int getgrouplist(const char *__user, __gid_t __group, __gid_t *__groups, int *__ngroups);
int getgroups(int __size, __gid_t *__list);
struct hostent *gethostbyaddr(const void *__addr, __socklen_t __len, int __type);
int gethostbyaddr_r(const void *__addr, __socklen_t __len, int __type, struct hostent *__result_buf, char *__buf, size_t __buflen, struct hostent **__result, int *__h_errnop);
struct hostent *gethostbyname(const char *__name);
struct hostent *gethostbyname2(const char *__name, int __af);
int gethostbyname2_r(const char *__name, int __af, struct hostent *__result_buf, char *__buf, size_t __buflen, struct hostent **__result, int *__h_errnop);
int gethostbyname_r(const char *__name, struct hostent *__result_buf, char *__buf, size_t __buflen, struct hostent **__result, int *__h_errnop);
struct hostent *gethostent(void);
int gethostent_r(struct hostent *__result_buf, char *__buf, size_t __buflen, struct hostent **__result, int *__h_errnop);
long gethostid(void);
int gethostname(char *__name, size_t __len);
int getifaddrs(struct ifaddrs **__ifap);
int getipv4sourcefilter(int __s, struct in_addr __interface_addr, struct in_addr __group, unsigned int *__fmode, unsigned int *__numsrc, struct in_addr *__slist);
int getitimer(__itimer_which_t __which, struct itimerval *__value);
__ssize_t getline(char **__lineptr, size_t *__n, FILE *__stream);
int getloadavg(double *__loadavg, int __nelem);
char *getlogin(void);
int getlogin_r(char *__name, size_t __name_len);
int getnameinfo(const struct sockaddr *__sa, socklen_t __salen, char *__host, socklen_t __hostlen, char *__serv, socklen_t __servlen, int __flags);
struct netent *getnetbyaddr(unsigned int __net, int __type);
int getnetbyaddr_r(unsigned int __net, int __type, struct netent *__result_buf, char *__buf, size_t __buflen, struct netent **__result, int *__h_errnop);
struct netent *getnetbyname(const char *__name);
int getnetbyname_r(const char *__name, struct netent *__result_buf, char *__buf, size_t __buflen, struct netent **__result, int *__h_errnop);
struct netent *getnetent(void);
int getnetent_r(struct netent *__result_buf, char *__buf, size_t __buflen, struct netent **__result, int *__h_errnop);
int getnetgrent(char **__hostp, char **__userp, char **__domainp);
int getnetgrent_r(char **__hostp, char **__userp, char **__domainp, char *__buffer, size_t __buflen);
int getopt(int ___argc, char *const *___argv, const char *__shortopts);
int getopt_long(int ___argc, char *const *___argv, const char *__shortopts, const struct option *__longopts, int *__longind);
int getopt_long_only(int ___argc, char *const *___argv, const char *__shortopts, const struct option *__longopts, int *__longind);
int getpagesize(void);
char *getpass(const char *__prompt);
double getpayload(const double *__x);
float getpayloadf(const float *__x);
_Float32 getpayloadf32(const _Float32 *__x);
_Float32x getpayloadf32x(const _Float32x *__x);
_Float64 getpayloadf64(const _Float64 *__x);
_Float64x getpayloadf64x(const _Float64x *__x);
long double getpayloadl(const long double *__x);
int getpeername(int __fd, union __SOCKADDR_ARG __addr, socklen_t *__len);
__pid_t getpgid(__pid_t __pid);
__pid_t getpgrp(void);
__pid_t getpid(void);
__pid_t getppid(void);
int getpriority(__priority_which_t __which, id_t __who);
struct protoent *getprotobyname(const char *__name);
int getprotobyname_r(const char *__name, struct protoent *__result_buf, char *__buf, size_t __buflen, struct protoent **__result);
struct protoent *getprotobynumber(int __proto);
int getprotobynumber_r(int __proto, struct protoent *__result_buf, char *__buf, size_t __buflen, struct protoent **__result);
struct protoent *getprotoent(void);
int getprotoent_r(struct protoent *__result_buf, char *__buf, size_t __buflen, struct protoent **__result);
int getpt(void);
int getpw(__uid_t __uid, char *__buffer);
struct passwd *getpwent(void);
int getpwent_r(struct passwd *__resultbuf, char *__buffer, size_t __buflen, struct passwd **__result);
struct passwd *getpwnam(const char *__name);
int getpwnam_r(const char *__name, struct passwd *__resultbuf, char *__buffer, size_t __buflen, struct passwd **__result);
struct passwd *getpwuid(__uid_t __uid);
int getpwuid_r(__uid_t __uid, struct passwd *__resultbuf, char *__buffer, size_t __buflen, struct passwd **__result);
ssize_t getrandom(void *__buffer, size_t __length, unsigned int __flags);
int getresgid(__gid_t *__rgid, __gid_t *__egid, __gid_t *__sgid);
int getresuid(__uid_t *__ruid, __uid_t *__euid, __uid_t *__suid);
int getrlimit(__rlimit_resource_t __resource, struct rlimit *__rlimits);
int getrlimit64(__rlimit_resource_t __resource, struct rlimit64 *__rlimits);
struct rpcent *getrpcbyname(const char *__name);
int getrpcbyname_r(const char *__name, struct rpcent *__result_buf, char *__buffer, size_t __buflen, struct rpcent **__result);
struct rpcent *getrpcbynumber(int __number);
int getrpcbynumber_r(int __number, struct rpcent *__result_buf, char *__buffer, size_t __buflen, struct rpcent **__result);
struct rpcent *getrpcent(void);
int getrpcent_r(struct rpcent *__result_buf, char *__buffer, size_t __buflen, struct rpcent **__result);
int getrusage(__rusage_who_t __who, struct rusage *__usage);
struct servent *getservbyname(const char *__name, const char *__proto);
int getservbyname_r(const char *__name, const char *__proto, struct servent *__result_buf, char *__buf, size_t __buflen, struct servent **__result);
struct servent *getservbyport(int __port, const char *__proto);
int getservbyport_r(int __port, const char *__proto, struct servent *__result_buf, char *__buf, size_t __buflen, struct servent **__result);
struct servent *getservent(void);
int getservent_r(struct servent *__result_buf, char *__buf, size_t __buflen, struct servent **__result);
__pid_t getsid(__pid_t __pid);
int getsockname(int __fd, union __SOCKADDR_ARG __addr, socklen_t *__len);
int getsockopt(int __fd, int __level, int __optname, void *__optval, socklen_t *__optlen);
int getsourcefilter(int __s, unsigned int __interface_addr, const struct sockaddr *__group, socklen_t __grouplen, unsigned int *__fmode, unsigned int *__numsrc, struct sockaddr_storage *__slist);
struct spwd *getspent(void);
int getspent_r(struct spwd *__result_buf, char *__buffer, size_t __buflen, struct spwd **__result);
struct spwd *getspnam(const char *__name);
int getspnam_r(const char *__name, struct spwd *__result_buf, char *__buffer, size_t __buflen, struct spwd **__result);
int getsubopt(char **__optionp, char *const *__tokens, char **__valuep);
__pid_t gettid(void);
int gettimeofday(struct timeval *__tv, void *__tz);
__uid_t getuid(void);
__mode_t getumask(void);
char *getusershell(void);
struct utmp *getutent(void);
int getutent_r(struct utmp *__buffer, struct utmp **__result);
struct utmp *getutid(const struct utmp *__id);
int getutid_r(const struct utmp *__id, struct utmp *__buffer, struct utmp **__result);
struct utmp *getutline(const struct utmp *__line);
int getutline_r(const struct utmp *__line, struct utmp *__buffer, struct utmp **__result);
int getw(FILE *__stream);
wint_t getwc(__FILE *__stream);
wint_t getwc_unlocked(__FILE *__stream);
wint_t getwchar(void);
wint_t getwchar_unlocked(void);
char *getwd(char *__buf);
ssize_t getxattr(const char *__path, const char *__name, void *__value, size_t __size);
int glob(const char *__pattern, int __flags, int (*__errfunc)(const char *, int), struct glob_t *__pglob);
int glob64(const char *__pattern, int __flags, int (*__errfunc)(const char *, int), struct glob64_t *__pglob);
int glob_pattern_p(const char *__pattern, int __quote);
void globfree(struct glob_t *__pglob);
void globfree64(struct glob64_t *__pglob);
struct tm *gmtime(const time_t *__timer);
struct tm *gmtime_r(const time_t *__timer, struct tm *__tp);
int grantpt(int __fd);
int group_member(__gid_t __gid);
int gsignal(int __sig);
int hcreate(size_t __nel);
int hcreate_r(size_t __nel, struct hsearch_data *__htab);
void hdestroy(void);
void hdestroy_r(struct hsearch_data *__htab);
void herror(const char *__str);
ENTRY *hsearch(ENTRY __item, enum ACTION __action);
int hsearch_r(ENTRY __item, enum ACTION __action, ENTRY **__retval, struct hsearch_data *__htab);
const char *hstrerror(int __err_num);
unsigned int htonl(unsigned int __hostlong);
unsigned short htons(unsigned short __hostshort);
double hypot(double __x, double __y);
float hypotf(float __x, float __y);
_Float32 hypotf32(_Float32 __x, _Float32 __y);
_Float32x hypotf32x(_Float32x __x, _Float32x __y);
_Float64 hypotf64(_Float64 __x, _Float64 __y);
_Float64x hypotf64x(_Float64x __x, _Float64x __y);
long double hypotl(long double __x, long double __y);
size_t iconv(iconv_t __cd, char **__inbuf, size_t *__inbytesleft, char **__outbuf, size_t *__outbytesleft);
int iconv_close(iconv_t __cd);
iconv_t iconv_open(const char *__tocode, const char *__fromcode);
void if_freenameindex(struct if_nameindex *__ptr);
char *if_indextoname(unsigned int __ifindex, char *__ifname);
struct if_nameindex *if_nameindex(void);
unsigned int if_nametoindex(const char *__ifname);
int ilogb(double __x);
int ilogbf(float __x);
int ilogbf32(_Float32 __x);
int ilogbf32x(_Float32x __x);
int ilogbf64(_Float64 __x);
int ilogbf64x(_Float64x __x);
int ilogbl(long double __x);
intmax_t imaxabs(intmax_t __n);
struct imaxdiv_t imaxdiv(intmax_t __numer, intmax_t __denom);
char *index(const char *__s, int __c);
int inet6_opt_append(void *__extbuf, socklen_t __extlen, int __offset, unsigned char __type, socklen_t __len, unsigned char __align, void **__databufp);
int inet6_opt_find(void *__extbuf, socklen_t __extlen, int __offset, unsigned char __type, socklen_t *__lenp, void **__databufp);
int inet6_opt_finish(void *__extbuf, socklen_t __extlen, int __offset);
int inet6_opt_get_val(void *__databuf, int __offset, void *__val, socklen_t __vallen);
int inet6_opt_init(void *__extbuf, socklen_t __extlen);
int inet6_opt_next(void *__extbuf, socklen_t __extlen, int __offset, unsigned char *__typep, socklen_t *__lenp, void **__databufp);
int inet6_opt_set_val(void *__databuf, int __offset, void *__val, socklen_t __vallen);
unsigned char *inet6_option_alloc(struct cmsghdr *__cmsg, int __datalen, int __multx, int __plusy);
int inet6_option_append(struct cmsghdr *__cmsg, const unsigned char *__typep, int __multx, int __plusy);
int inet6_option_find(const struct cmsghdr *__cmsg, unsigned char **__tptrp, int __type);
int inet6_option_init(void *__bp, struct cmsghdr **__cmsgp, int __type);
int inet6_option_next(const struct cmsghdr *__cmsg, unsigned char **__tptrp);
int inet6_option_space(int __nbytes);
int inet6_rth_add(void *__bp, const struct in6_addr *__addr);
struct in6_addr *inet6_rth_getaddr(const void *__bp, int __index);
void *inet6_rth_init(void *__bp, socklen_t __bp_len, int __type, int __segments);
int inet6_rth_reverse(const void *__in, void *__out);
int inet6_rth_segments(const void *__bp);
socklen_t inet6_rth_space(int __type, int __segments);
in_addr_t inet_addr(const char *__cp);
int inet_aton(const char *__cp, struct in_addr *__inp);
in_addr_t inet_lnaof(struct in_addr __in);
struct in_addr inet_makeaddr(in_addr_t __net, in_addr_t __host);
char *inet_net_ntop(int __af, const void *__cp, int __bits, char *__buf, size_t __len);
int inet_net_pton(int __af, const char *__cp, void *__buf, size_t __len);
char *inet_neta(in_addr_t __net, char *__buf, size_t __len);
in_addr_t inet_netof(struct in_addr __in);
in_addr_t inet_network(const char *__cp);
unsigned int inet_nsap_addr(const char *__cp, unsigned char *__buf, int __len);
char *inet_nsap_ntoa(int __len, const unsigned char *__cp, char *__buf);
char *inet_ntoa(struct in_addr __in);
const char *inet_ntop(int __af, const void *__cp, char *__buf, socklen_t __len);
int inet_pton(int __af, const char *__cp, void *__buf);
int initgroups(const char *__user, __gid_t __group);
char *initstate(unsigned int __seed, char *__statebuf, size_t __statelen);
int initstate_r(unsigned int __seed, char *__statebuf, size_t __statelen, struct random_data *__buf);
int innetgr(const char *__netgroup, const char *__host, const char *__user, const char *__domain);
int inotify_add_watch(int __fd, const char *__name, unsigned int __mask);
int inotify_init(void);
int inotify_init1(int __flags);
int inotify_rm_watch(int __fd, int __wd);
void insque(void *__elem, void *__prev);
int ioctl(int __fd, unsigned long __request, ...);
int iruserok(unsigned int __raddr, int __suser, const char *__remuser, const char *__locuser);
int iruserok_af(const void *__raddr, int __suser, const char *__remuser, const char *__locuser, sa_family_t __af);
int isalnum(int);
int isalnum_l(int, locale_t);
int isalpha(int);
int isalpha_l(int, locale_t);
int isascii(int __c);
int isatty(int __fd);
int isblank(int);
int isblank_l(int, locale_t);
int iscntrl(int);
int iscntrl_l(int, locale_t);
int isctype(int __c, int __mask);
int isdigit(int);
int isdigit_l(int, locale_t);
int isfdtype(int __fd, int __fdtype);
int isgraph(int);
int isgraph_l(int, locale_t);
int isinf(double __value);
int isinff(float __value);
int isinfl(long double __value);
int islower(int);
int islower_l(int, locale_t);
int isnan(double __value);
int isnanf(float __value);
int isnanl(long double __value);
int isprint(int);
int isprint_l(int, locale_t);
int ispunct(int);
int ispunct_l(int, locale_t);
int isspace(int);
int isspace_l(int, locale_t);
int isupper(int);
int isupper_l(int, locale_t);
int iswalnum(wint_t __wc);
int iswalnum_l(wint_t __wc, locale_t __locale);
int iswalpha(wint_t __wc);
int iswalpha_l(wint_t __wc, locale_t __locale);
int iswblank(wint_t __wc);
int iswblank_l(wint_t __wc, locale_t __locale);
int iswcntrl(wint_t __wc);
int iswcntrl_l(wint_t __wc, locale_t __locale);
int iswctype(wint_t __wc, wctype_t __desc);
int iswctype_l(wint_t __wc, wctype_t __desc, locale_t __locale);
int iswdigit(wint_t __wc);
int iswdigit_l(wint_t __wc, locale_t __locale);
int iswgraph(wint_t __wc);
int iswgraph_l(wint_t __wc, locale_t __locale);
int iswlower(wint_t __wc);
int iswlower_l(wint_t __wc, locale_t __locale);
int iswprint(wint_t __wc);
int iswprint_l(wint_t __wc, locale_t __locale);
int iswpunct(wint_t __wc);
int iswpunct_l(wint_t __wc, locale_t __locale);
int iswspace(wint_t __wc);
int iswspace_l(wint_t __wc, locale_t __locale);
int iswupper(wint_t __wc);
int iswupper_l(wint_t __wc, locale_t __locale);
int iswxdigit(wint_t __wc);
int iswxdigit_l(wint_t __wc, locale_t __locale);
int isxdigit(int);
int isxdigit_l(int, locale_t);
double j0(double);
float j0f(float);
_Float32 j0f32(_Float32);
_Float32x j0f32x(_Float32x);
_Float64 j0f64(_Float64);
_Float64x j0f64x(_Float64x);
long double j0l(long double);
double j1(double);
float j1f(float);
_Float32 j1f32(_Float32);
_Float32x j1f32x(_Float32x);
_Float64 j1f64(_Float64);
_Float64x j1f64x(_Float64x);
long double j1l(long double);
double jn(int, double);
float jnf(int, float);
_Float32 jnf32(int, _Float32);
_Float32x jnf32x(int, _Float32x);
_Float64 jnf64(int, _Float64);
_Float64x jnf64x(int, _Float64x);
long double jnl(int, long double);
long jrand48(unsigned short *__xsubi);
int jrand48_r(unsigned short *__xsubi, struct drand48_data *__buffer, long *__result);
int kill(__pid_t __pid, int __sig);
int killpg(__pid_t __pgrp, int __sig);
char *l64a(long __n);
void la_activity(uintptr_t *__cookie, unsigned int __flag);
Elf32_Addr la_i86_gnu_pltenter(struct Elf32_Sym *__sym, unsigned int __ndx, uintptr_t *__refcook, uintptr_t *__defcook, struct La_i86_regs *__regs, unsigned int *__flags, const char *__symname, long *__framesizep);
unsigned int la_i86_gnu_pltexit(struct Elf32_Sym *__sym, unsigned int __ndx, uintptr_t *__refcook, uintptr_t *__defcook, const struct La_i86_regs *__inregs, struct La_i86_retval *__outregs, const char *symname);
unsigned int la_objclose(uintptr_t *__cookie);
unsigned int la_objopen(struct link_map *__map, Lmid_t __lmid, uintptr_t *__cookie);
char *la_objsearch(const char *__name, uintptr_t *__cookie, unsigned int __flag);
void la_preinit(uintptr_t *__cookie);
uintptr_t la_symbind32(struct Elf32_Sym *__sym, unsigned int __ndx, uintptr_t *__refcook, uintptr_t *__defcook, unsigned int *__flags, const char *__symname);
uintptr_t la_symbind64(struct Elf64_Sym *__sym, unsigned int __ndx, uintptr_t *__refcook, uintptr_t *__defcook, unsigned int *__flags, const char *__symname);
unsigned int la_version(unsigned int __version);
long labs(long __x);
int lchmod(const char *__file, __mode_t __mode);
int lchown(const char *__file, __uid_t __owner, __gid_t __group);
int lckpwdf(void);
void lcong48(unsigned short *__param);
int lcong48_r(unsigned short *__param, struct drand48_data *__buffer);
double ldexp(double __x, int __exponent);
float ldexpf(float __x, int __exponent);
_Float32 ldexpf32(_Float32 __x, int __exponent);
_Float32x ldexpf32x(_Float32x __x, int __exponent);
_Float64 ldexpf64(_Float64 __x, int __exponent);
_Float64x ldexpf64x(_Float64x __x, int __exponent);
long double ldexpl(long double __x, int __exponent);
struct ldiv_t ldiv(long __numer, long __denom);
void *lfind(const void *__key, const void *__base, size_t *__nmemb, size_t __size, __compar_fn_t __compar);
double lgamma(double);
double lgamma_r(double, int *__signgamp);
float lgammaf(float);
_Float32 lgammaf32(_Float32);
_Float32 lgammaf32_r(_Float32, int *__signgamp);
_Float32x lgammaf32x(_Float32x);
_Float32x lgammaf32x_r(_Float32x, int *__signgamp);
_Float64 lgammaf64(_Float64);
_Float64 lgammaf64_r(_Float64, int *__signgamp);
_Float64x lgammaf64x(_Float64x);
_Float64x lgammaf64x_r(_Float64x, int *__signgamp);
float lgammaf_r(float, int *__signgamp);
long double lgammal(long double);
long double lgammal_r(long double, int *__signgamp);
ssize_t lgetxattr(const char *__path, const char *__name, void *__value, size_t __size);
int link(const char *__from, const char *__to);
int linkat(int __fromfd, const char *__from, int __tofd, const char *__to, int __flags);
int lio_listio(int __mode, struct aiocb *const *__list, int __nent, struct sigevent *__sig);
int lio_listio64(int __mode, struct aiocb64 *const *__list, int __nent, struct sigevent *__sig);
int listen(int __fd, int __n);
ssize_t listxattr(const char *__path, char *__list, size_t __size);
long long llabs(long long __x);
struct lldiv_t lldiv(long long __numer, long long __denom);
ssize_t llistxattr(const char *__path, char *__list, size_t __size);
long llogb(double __x);
long llogbf(float __x);
long llogbf32(_Float32 __x);
long llogbf32x(_Float32x __x);
long llogbf64(_Float64 __x);
long llogbf64x(_Float64x __x);
long llogbl(long double __x);
long long llrint(double __x);
long long llrintf(float __x);
long long llrintf32(_Float32 __x);
long long llrintf32x(_Float32x __x);
long long llrintf64(_Float64 __x);
long long llrintf64x(_Float64x __x);
long long llrintl(long double __x);
long long llround(double __x);
long long llroundf(float __x);
long long llroundf32(_Float32 __x);
long long llroundf32x(_Float32x __x);
long long llroundf64(_Float64 __x);
long long llroundf64x(_Float64x __x);
long long llroundl(long double __x);
struct lconv *localeconv(void);
struct tm *localtime(const time_t *__timer);
struct tm *localtime_r(const time_t *__timer, struct tm *__tp);
int lockf(int __fd, int __cmd, __off_t __len);
int lockf64(int __fd, int __cmd, __off64_t __len);
double log(double __x);
double log10(double __x);
float log10f(float __x);
_Float32 log10f32(_Float32 __x);
_Float32x log10f32x(_Float32x __x);
_Float64 log10f64(_Float64 __x);
_Float64x log10f64x(_Float64x __x);
long double log10l(long double __x);
double log10p1(double __x);
float log10p1f(float __x);
_Float32 log10p1f32(_Float32 __x);
_Float32x log10p1f32x(_Float32x __x);
_Float64 log10p1f64(_Float64 __x);
_Float64x log10p1f64x(_Float64x __x);
long double log10p1l(long double __x);
double log1p(double __x);
float log1pf(float __x);
_Float32 log1pf32(_Float32 __x);
_Float32x log1pf32x(_Float32x __x);
_Float64 log1pf64(_Float64 __x);
_Float64x log1pf64x(_Float64x __x);
long double log1pl(long double __x);
double log2(double __x);
float log2f(float __x);
_Float32 log2f32(_Float32 __x);
_Float32x log2f32x(_Float32x __x);
_Float64 log2f64(_Float64 __x);
_Float64x log2f64x(_Float64x __x);
long double log2l(long double __x);
double log2p1(double __x);
float log2p1f(float __x);
_Float32 log2p1f32(_Float32 __x);
_Float32x log2p1f32x(_Float32x __x);
_Float64 log2p1f64(_Float64 __x);
_Float64x log2p1f64x(_Float64x __x);
long double log2p1l(long double __x);
double logb(double __x);
float logbf(float __x);
_Float32 logbf32(_Float32 __x);
_Float32x logbf32x(_Float32x __x);
_Float64 logbf64(_Float64 __x);
_Float64x logbf64x(_Float64x __x);
long double logbl(long double __x);
float logf(float __x);
_Float32 logf32(_Float32 __x);
_Float32x logf32x(_Float32x __x);
_Float64 logf64(_Float64 __x);
_Float64x logf64x(_Float64x __x);
void login(const struct utmp *__entry);
int login_tty(int __fd);
long double logl(long double __x);
int logout(const char *__ut_line);
double logp1(double __x);
float logp1f(float __x);
_Float32 logp1f32(_Float32 __x);
_Float32x logp1f32x(_Float32x __x);
_Float64 logp1f64(_Float64 __x);
_Float64x logp1f64x(_Float64x __x);
long double logp1l(long double __x);
void logwtmp(const char *__ut_line, const char *__ut_name, const char *__ut_host);
_Noreturn void longjmp(struct __jmp_buf_tag *__env, int __val);
long lrand48(void);
int lrand48_r(struct drand48_data *__buffer, long *__result);
int lremovexattr(const char *__path, const char *__name);
long lrint(double __x);
long lrintf(float __x);
long lrintf32(_Float32 __x);
long lrintf32x(_Float32x __x);
long lrintf64(_Float64 __x);
long lrintf64x(_Float64x __x);
long lrintl(long double __x);
long lround(double __x);
long lroundf(float __x);
long lroundf32(_Float32 __x);
long lroundf32x(_Float32x __x);
long lroundf64(_Float64 __x);
long lroundf64x(_Float64x __x);
long lroundl(long double __x);
void *lsearch(const void *__key, void *__base, size_t *__nmemb, size_t __size, __compar_fn_t __compar);
__off_t lseek(int __fd, __off_t __offset, int __whence);
__off64_t lseek64(int __fd, __off64_t __offset, int __whence);
int lsetxattr(const char *__path, const char *__name, const void *__value, size_t __size, int __flags);
int lstat(const char *__file, struct stat *__buf);
int lstat64(const char *__file, struct stat64 *__buf);
int lutimes(const char *__file, const struct timeval *__tvp);
int madvise(void *__addr, size_t __len, int __advice);
void makecontext(struct ucontext_t *__ucp, void (*__func)(void), int __argc, ...);
struct mallinfo mallinfo(void);
struct mallinfo2 mallinfo2(void);
void *malloc(size_t __size);
int malloc_info(int __options, FILE *__fp);
void malloc_stats(void);
int malloc_trim(size_t __pad);
size_t malloc_usable_size(void *__ptr);
int mallopt(int __param, int __val);
int mblen(const char *__s, size_t __n);
size_t mbrlen(const char *__s, size_t __n, mbstate_t *__ps);
size_t mbrtowc(int *__pwc, const char *__s, size_t __n, mbstate_t *__p);
int mbsinit(const mbstate_t *__ps);
size_t mbsnrtowcs(int *__dst, const char **__src, size_t __nmc, size_t __len, mbstate_t *__ps);
size_t mbsrtowcs(int *__dst, const char **__src, size_t __len, mbstate_t *__ps);
size_t mbstowcs(int *__pwcs, const char *__s, size_t __n);
int mbtowc(int *__pwc, const char *__s, size_t __n);
void *memalign(size_t __alignment, size_t __size);
size_t memalignment(const void *__p);
void *memccpy(void *__dest, const void *__src, int __c, size_t __n);
void *memchr(const void *__s, int __c, size_t __n);
int memcmp(const void *__s1, const void *__s2, size_t __n);
void *memcpy(void *__dest, const void *__src, size_t __n);
int memfd_create(const char *__name, unsigned int __flags);
void *memfrob(void *__s, size_t __n);
void *memmem(const void *__haystack, size_t __haystacklen, const void *__needle, size_t __needlelen);
void *memmove(void *__dest, const void *__src, size_t __n);
void *mempcpy(void *__dest, const void *__src, size_t __n);
void *memrchr(const void *__s, int __c, size_t __n);
void *memset(void *__s, int __c, size_t __n);
void *memset_explicit(void *__s, int __c, size_t __n);
int mincore(void *__start, size_t __len, unsigned char *__vec);
int mkdir(const char *__path, __mode_t __mode);
int mkdirat(int __fd, const char *__path, __mode_t __mode);
char *mkdtemp(char *__template);
int mkfifo(const char *__path, __mode_t __mode);
int mkfifoat(int __fd, const char *__path, __mode_t __mode);
int mknod(const char *__path, __mode_t __mode, __dev_t __dev);
int mknodat(int __fd, const char *__path, __mode_t __mode, __dev_t __dev);
int mkostemp(char *__template, int __flags);
int mkostemp64(char *__template, int __flags);
int mkostemps(char *__template, int __suffixlen, int __flags);
int mkostemps64(char *__template, int __suffixlen, int __flags);
int mkstemp(char *__template);
int mkstemp64(char *__template);
int mkstemps(char *__template, int __suffixlen);
int mkstemps64(char *__template, int __suffixlen);
char *mktemp(char *__template);
time_t mktime(struct tm *__tp);
int mlock(const void *__addr, size_t __len);
int mlock2(const void *__addr, size_t __length, unsigned int __flags);
int mlockall(int __flags);
void *mmap(void *__addr, size_t __len, int __prot, int __flags, int __fd, __off_t __offset);
void *mmap64(void *__addr, size_t __len, int __prot, int __flags, int __fd, __off64_t __offset);
double modf(double __x, double *__iptr);
float modff(float __x, float *__iptr);
_Float32 modff32(_Float32 __x, _Float32 *__iptr);
_Float32x modff32x(_Float32x __x, _Float32x *__iptr);
_Float64 modff64(_Float64 __x, _Float64 *__iptr);
_Float64x modff64x(_Float64x __x, _Float64x *__iptr);
long double modfl(long double __x, long double *__iptr);
int mprotect(void *__addr, size_t __len, int __prot);
int mq_close(mqd_t __mqdes);
int mq_getattr(mqd_t __mqdes, struct mq_attr *__mqstat);
int mq_notify(mqd_t __mqdes, const struct sigevent *__notification);
mqd_t mq_open(const char *__name, int __oflag, ...);
ssize_t mq_receive(mqd_t __mqdes, char *__msg_ptr, size_t __msg_len, unsigned int *__msg_prio);
int mq_send(mqd_t __mqdes, const char *__msg_ptr, size_t __msg_len, unsigned int __msg_prio);
int mq_setattr(mqd_t __mqdes, const struct mq_attr *__mqstat, struct mq_attr *__omqstat);
ssize_t mq_timedreceive(mqd_t __mqdes, char *__msg_ptr, size_t __msg_len, unsigned int *__msg_prio, const struct timespec *__abs_timeout);
int mq_timedsend(mqd_t __mqdes, const char *__msg_ptr, size_t __msg_len, unsigned int __msg_prio, const struct timespec *__abs_timeout);
int mq_unlink(const char *__name);
long mrand48(void);
int mrand48_r(struct drand48_data *__buffer, long *__result);
void *mremap(void *__addr, size_t __old_len, size_t __new_len, int __flags, ...);
int mseal(void *__addr, size_t __len, unsigned long flags);
int msgctl(int __msqid, int __cmd, struct msqid_ds *__buf);
int msgget(key_t __key, int __msgflg);
ssize_t msgrcv(int __msqid, void *__msgp, size_t __msgsz, long __msgtyp, int __msgflg);
int msgsnd(int __msqid, const void *__msgp, size_t __msgsz, int __msgflg);
int msync(void *__addr, size_t __len, int __flags);
int munlock(const void *__addr, size_t __len);
int munlockall(void);
int munmap(void *__addr, size_t __len);
int name_to_handle_at(int __dfd, const char *__name, struct file_handle *__handle, int *__mnt_id, int __flags);
double nan(const char *__tagb);
float nanf(const char *__tagb);
_Float32 nanf32(const char *__tagb);
_Float32x nanf32x(const char *__tagb);
_Float64 nanf64(const char *__tagb);
_Float64x nanf64x(const char *__tagb);
long double nanl(const char *__tagb);
int nanosleep(const struct timespec *__requested_time, struct timespec *__remaining);
double nearbyint(double __x);
float nearbyintf(float __x);
_Float32 nearbyintf32(_Float32 __x);
_Float32x nearbyintf32x(_Float32x __x);
_Float64 nearbyintf64(_Float64 __x);
_Float64x nearbyintf64x(_Float64x __x);
long double nearbyintl(long double __x);
locale_t newlocale(int __category_mask, const char *__locale, locale_t __base);
double nextafter(double __x, double __y);
float nextafterf(float __x, float __y);
_Float32 nextafterf32(_Float32 __x, _Float32 __y);
_Float32x nextafterf32x(_Float32x __x, _Float32x __y);
_Float64 nextafterf64(_Float64 __x, _Float64 __y);
_Float64x nextafterf64x(_Float64x __x, _Float64x __y);
long double nextafterl(long double __x, long double __y);
double nextdown(double __x);
float nextdownf(float __x);
_Float32 nextdownf32(_Float32 __x);
_Float32x nextdownf32x(_Float32x __x);
_Float64 nextdownf64(_Float64 __x);
_Float64x nextdownf64x(_Float64x __x);
long double nextdownl(long double __x);
double nexttoward(double __x, long double __y);
float nexttowardf(float __x, long double __y);
long double nexttowardl(long double __x, long double __y);
double nextup(double __x);
float nextupf(float __x);
_Float32 nextupf32(_Float32 __x);
_Float32x nextupf32x(_Float32x __x);
_Float64 nextupf64(_Float64 __x);
_Float64x nextupf64x(_Float64x __x);
long double nextupl(long double __x);
int nftw(const char *__dir, __nftw_func_t __func, int __descriptors, int __flag);
int nftw64(const char *__dir, __nftw64_func_t __func, int __descriptors, int __flag);
int nice(int __inc);
char *nl_langinfo(nl_item __item);
char *nl_langinfo_l(nl_item __item, locale_t __l);
long nrand48(unsigned short *__xsubi);
int nrand48_r(unsigned short *__xsubi, struct drand48_data *__buffer, long *__result);
unsigned int ntohl(unsigned int __netlong);
unsigned short ntohs(unsigned short __netshort);
int obstack_printf(struct obstack *__obstack, const char *__format, ...);
int obstack_vprintf(struct obstack *__obstack, const char *__format, char *__args);
int on_exit(void (*__func)(int __status, void *__arg), void *__arg);
int open(const char *__file, int __oflag, ...);
int open64(const char *__file, int __oflag, ...);
int open_by_handle_at(int __mountdirfd, struct file_handle *__handle, int __flags);
FILE *open_memstream(char **__bufloc, size_t *__sizeloc);
__FILE *open_wmemstream(int **__bufloc, size_t *__sizeloc);
int openat(int __fd, const char *__file, int __oflag, ...);
int openat2(int __dfd, const char *__filename, const struct open_how *__how, unsigned int __usize);
int openat64(int __fd, const char *__file, int __oflag, ...);
DIR *opendir(const char *__name);
void openlog(const char *__ident, int __option, int __facility);
long pathconf(const char *__path, int __name);
int pause(void);
int pclose(FILE *__stream);
void perror(const char *__s);
int pidfd_spawn(int *__pidfd, const char *__path, const struct posix_spawn_file_actions_t *__facts, const struct posix_spawnattr_t *__attrp, char *const *__argv, char *const *__envp);
int pidfd_spawnp(int *__pidfd, const char *__file, const struct posix_spawn_file_actions_t *__facts, const struct posix_spawnattr_t *__attrp, char *const *__argv, char *const *__envp);
int pipe(int *__pipedes);
int pipe2(int *__pipedes, int __flags);
int pkey_alloc(unsigned int __flags, unsigned int __access_restrictions);
int pkey_free(int __key);
int pkey_get(int __key);
int pkey_mprotect(void *__addr, size_t __len, int __prot, int __pkey);
int pkey_set(int __key, unsigned int __access_restrictions);
int poll(struct pollfd *__fds, nfds_t __nfds, int __timeout);
FILE *popen(const char *__command, const char *__modes);
int posix_fadvise(int __fd, off_t __offset, off_t __len, int __advise);
int posix_fadvise64(int __fd, off64_t __offset, off64_t __len, int __advise);
int posix_fallocate(int __fd, off_t __offset, off_t __len);
int posix_fallocate64(int __fd, off64_t __offset, off64_t __len);
int posix_madvise(void *__addr, size_t __len, int __advice);
int posix_memalign(void **__memptr, size_t __alignment, size_t __size);
int posix_openpt(int __oflag);
int posix_spawn(pid_t *__pid, const char *__path, const struct posix_spawn_file_actions_t *__file_actions, const struct posix_spawnattr_t *__attrp, char *const *__argv, char *const *__envp);
int posix_spawn_file_actions_addchdir_np(struct posix_spawn_file_actions_t *__actions, const char *__path);
int posix_spawn_file_actions_addclose(struct posix_spawn_file_actions_t *__file_actions, int __fd);
int posix_spawn_file_actions_addclosefrom_np(struct posix_spawn_file_actions_t *, int __from);
int posix_spawn_file_actions_adddup2(struct posix_spawn_file_actions_t *__file_actions, int __fd, int __newfd);
int posix_spawn_file_actions_addfchdir_np(struct posix_spawn_file_actions_t *, int __fd);
int posix_spawn_file_actions_addopen(struct posix_spawn_file_actions_t *__file_actions, int __fd, const char *__path, int __oflag, mode_t __mode);
int posix_spawn_file_actions_addtcsetpgrp_np(struct posix_spawn_file_actions_t *, int __tcfd);
int posix_spawn_file_actions_destroy(struct posix_spawn_file_actions_t *__file_actions);
int posix_spawn_file_actions_init(struct posix_spawn_file_actions_t *__file_actions);
int posix_spawnattr_destroy(struct posix_spawnattr_t *__attr);
int posix_spawnattr_getcgroup_np(const struct posix_spawnattr_t *__attr, int *__cgroup);
int posix_spawnattr_getflags(const struct posix_spawnattr_t *__attr, short *__flags);
int posix_spawnattr_getpgroup(const struct posix_spawnattr_t *__attr, pid_t *__pgroup);
int posix_spawnattr_getschedparam(const struct posix_spawnattr_t *__attr, struct sched_param *__schedparam);
int posix_spawnattr_getschedpolicy(const struct posix_spawnattr_t *__attr, int *__schedpolicy);
int posix_spawnattr_getsigdefault(const struct posix_spawnattr_t *__attr, sigset_t *__sigdefault);
int posix_spawnattr_getsigmask(const struct posix_spawnattr_t *__attr, sigset_t *__sigmask);
int posix_spawnattr_init(struct posix_spawnattr_t *__attr);
int posix_spawnattr_setcgroup_np(struct posix_spawnattr_t *__attr, int __cgroup);
int posix_spawnattr_setflags(struct posix_spawnattr_t *_attr, short __flags);
int posix_spawnattr_setpgroup(struct posix_spawnattr_t *__attr, pid_t __pgroup);
int posix_spawnattr_setschedparam(struct posix_spawnattr_t *__attr, const struct sched_param *__schedparam);
int posix_spawnattr_setschedpolicy(struct posix_spawnattr_t *__attr, int __schedpolicy);
int posix_spawnattr_setsigdefault(struct posix_spawnattr_t *__attr, const sigset_t *__sigdefault);
int posix_spawnattr_setsigmask(struct posix_spawnattr_t *__attr, const sigset_t *__sigmask);
int posix_spawnp(pid_t *__pid, const char *__file, const struct posix_spawn_file_actions_t *__file_actions, const struct posix_spawnattr_t *__attrp, char *const *__argv, char *const *__envp);
double pow(double __x, double __y);
float powf(float __x, float __y);
_Float32 powf32(_Float32 __x, _Float32 __y);
_Float32x powf32x(_Float32x __x, _Float32x __y);
_Float64 powf64(_Float64 __x, _Float64 __y);
_Float64x powf64x(_Float64x __x, _Float64x __y);
long double powl(long double __x, long double __y);
double pown(double __x, long long __y);
float pownf(float __x, long long __y);
_Float32 pownf32(_Float32 __x, long long __y);
_Float32x pownf32x(_Float32x __x, long long __y);
_Float64 pownf64(_Float64 __x, long long __y);
_Float64x pownf64x(_Float64x __x, long long __y);
long double pownl(long double __x, long long __y);
double powr(double __x, double __y);
float powrf(float __x, float __y);
_Float32 powrf32(_Float32 __x, _Float32 __y);
_Float32x powrf32x(_Float32x __x, _Float32x __y);
_Float64 powrf64(_Float64 __x, _Float64 __y);
_Float64x powrf64x(_Float64x __x, _Float64x __y);
long double powrl(long double __x, long double __y);
int ppoll(struct pollfd *__fds, nfds_t __nfds, const struct timespec *__timeout, const struct __sigset_t *__ss);
int prctl(int __option, ...);
ssize_t pread(int __fd, void *__buf, size_t __nbytes, __off_t __offset);
ssize_t pread64(int __fd, void *__buf, size_t __nbytes, __off64_t __offset);
ssize_t preadv(int __fd, const struct iovec *__iovec, int __count, __off_t __offset);
ssize_t preadv2(int __fp, const struct iovec *__iovec, int __count, __off_t __offset, int ___flags);
ssize_t preadv64(int __fd, const struct iovec *__iovec, int __count, __off64_t __offset);
ssize_t preadv64v2(int __fp, const struct iovec *__iovec, int __count, __off64_t __offset, int ___flags);
int printf(const char *__format, ...);
int prlimit(__pid_t __pid, enum __rlimit_resource __resource, const struct rlimit *__new_limit, struct rlimit *__old_limit);
int prlimit64(__pid_t __pid, enum __rlimit_resource __resource, const struct rlimit64 *__new_limit, struct rlimit64 *__old_limit);
__ssize_t process_madvise(int __pid_fd, const struct iovec *__iov, size_t __count, int __advice, unsigned int __flags);
int process_mrelease(int pidfd, unsigned int flags);
ssize_t process_vm_readv(pid_t __pid, const struct iovec *__lvec, unsigned long __liovcnt, const struct iovec *__rvec, unsigned long __riovcnt, unsigned long __flags);
ssize_t process_vm_writev(pid_t __pid, const struct iovec *__lvec, unsigned long __liovcnt, const struct iovec *__rvec, unsigned long __riovcnt, unsigned long __flags);
int profil(unsigned short *__sample_buffer, size_t __size, size_t __offset, unsigned int __scale);
int pselect(int __nfds, struct fd_set *__readfds, struct fd_set *__writefds, struct fd_set *__exceptfds, const struct timespec *__timeout, const struct __sigset_t *__sigmask);
void psiginfo(const struct siginfo_t *__pinfo, const char *__s);
void psignal(int __sig, const char *__s);
int pthread_atfork(void (*__prepare)(void), void (*__parent)(void), void (*__child)(void));
int pthread_attr_destroy(union pthread_attr_t *__attr);
int pthread_attr_getaffinity_np(const union pthread_attr_t *__attr, size_t __cpusetsize, struct cpu_set_t *__cpuset);
int pthread_attr_getdetachstate(const union pthread_attr_t *__attr, int *__detachstate);
int pthread_attr_getguardsize(const union pthread_attr_t *__attr, size_t *__guardsize);
int pthread_attr_getinheritsched(const union pthread_attr_t *__attr, int *__inherit);
int pthread_attr_getschedparam(const union pthread_attr_t *__attr, struct sched_param *__param);
int pthread_attr_getschedpolicy(const union pthread_attr_t *__attr, int *__policy);
int pthread_attr_getscope(const union pthread_attr_t *__attr, int *__scope);
int pthread_attr_getsigmask_np(const union pthread_attr_t *__attr, struct __sigset_t *sigmask);
int pthread_attr_getstack(const union pthread_attr_t *__attr, void **__stackaddr, size_t *__stacksize);
int pthread_attr_getstackaddr(const union pthread_attr_t *__attr, void **__stackaddr);
int pthread_attr_getstacksize(const union pthread_attr_t *__attr, size_t *__stacksize);
int pthread_attr_init(union pthread_attr_t *__attr);
int pthread_attr_setaffinity_np(union pthread_attr_t *__attr, size_t __cpusetsize, const struct cpu_set_t *__cpuset);
int pthread_attr_setdetachstate(union pthread_attr_t *__attr, int __detachstate);
int pthread_attr_setguardsize(union pthread_attr_t *__attr, size_t __guardsize);
int pthread_attr_setinheritsched(union pthread_attr_t *__attr, int __inherit);
int pthread_attr_setschedparam(union pthread_attr_t *__attr, const struct sched_param *__param);
int pthread_attr_setschedpolicy(union pthread_attr_t *__attr, int __policy);
int pthread_attr_setscope(union pthread_attr_t *__attr, int __scope);
int pthread_attr_setsigmask_np(union pthread_attr_t *__attr, const struct __sigset_t *sigmask);
int pthread_attr_setstack(union pthread_attr_t *__attr, void *__stackaddr, size_t __stacksize);
int pthread_attr_setstackaddr(union pthread_attr_t *__attr, void *__stackaddr);
int pthread_attr_setstacksize(union pthread_attr_t *__attr, size_t __stacksize);
int pthread_barrier_destroy(union pthread_barrier_t *__barrier);
int pthread_barrier_init(union pthread_barrier_t *__barrier, const union pthread_barrierattr_t *__attr, unsigned int __count);
int pthread_barrier_wait(union pthread_barrier_t *__barrier);
int pthread_barrierattr_destroy(union pthread_barrierattr_t *__attr);
int pthread_barrierattr_getpshared(const union pthread_barrierattr_t *__attr, int *__pshared);
int pthread_barrierattr_init(union pthread_barrierattr_t *__attr);
int pthread_barrierattr_setpshared(union pthread_barrierattr_t *__attr, int __pshared);
int pthread_cancel(pthread_t __th);
int pthread_clockjoin_np(pthread_t __th, void **__thread_return, clockid_t __clockid, const struct timespec *__abstime);
int pthread_cond_broadcast(union pthread_cond_t *__cond);
int pthread_cond_clockwait(union pthread_cond_t *__cond, union pthread_mutex_t *__mutex, __clockid_t __clock_id, const struct timespec *__abstime);
int pthread_cond_destroy(union pthread_cond_t *__cond);
int pthread_cond_init(union pthread_cond_t *__cond, const union pthread_condattr_t *__cond_attr);
int pthread_cond_signal(union pthread_cond_t *__cond);
int pthread_cond_timedwait(union pthread_cond_t *__cond, union pthread_mutex_t *__mutex, const struct timespec *__abstime);
int pthread_cond_wait(union pthread_cond_t *__cond, union pthread_mutex_t *__mutex);
int pthread_condattr_destroy(union pthread_condattr_t *__attr);
int pthread_condattr_getclock(const union pthread_condattr_t *__attr, __clockid_t *__clock_id);
int pthread_condattr_getpshared(const union pthread_condattr_t *__attr, int *__pshared);
int pthread_condattr_init(union pthread_condattr_t *__attr);
int pthread_condattr_setclock(union pthread_condattr_t *__attr, __clockid_t __clock_id);
int pthread_condattr_setpshared(union pthread_condattr_t *__attr, int __pshared);
int pthread_create(pthread_t *__newthread, const union pthread_attr_t *__attr, void *(*__start_routine)(void *), void *__arg);
int pthread_detach(pthread_t __th);
int pthread_equal(pthread_t __thread1, pthread_t __thread2);
_Noreturn void pthread_exit(void *__retval);
int pthread_getaffinity_np(pthread_t __th, size_t __cpusetsize, struct cpu_set_t *__cpuset);
int pthread_getattr_default_np(union pthread_attr_t *__attr);
int pthread_getattr_np(pthread_t __th, union pthread_attr_t *__attr);
int pthread_getconcurrency(void);
int pthread_getcpuclockid(pthread_t __thread_id, __clockid_t *__clock_id);
int pthread_getname_np(pthread_t __target_thread, char *__buf, size_t __buflen);
int pthread_getschedparam(pthread_t __target_thread, int *__policy, struct sched_param *__param);
void *pthread_getspecific(pthread_key_t __key);
pid_t pthread_gettid_np(pthread_t __thread_id);
int pthread_join(pthread_t __th, void **__thread_return);
int pthread_key_create(pthread_key_t *__key, void (*__destr_function)(void *));
int pthread_key_delete(pthread_key_t __key);
int pthread_kill(pthread_t __threadid, int __signo);
int pthread_mutex_clocklock(union pthread_mutex_t *__mutex, clockid_t __clockid, const struct timespec *__abstime);
int pthread_mutex_consistent(union pthread_mutex_t *);
int pthread_mutex_consistent_np(union pthread_mutex_t *);
int pthread_mutex_destroy(union pthread_mutex_t *__mutex);
int pthread_mutex_getprioceiling(const union pthread_mutex_t *__mutex, int *__prioceiling);
int pthread_mutex_init(union pthread_mutex_t *__mutex, const union pthread_mutexattr_t *__mutexattr);
int pthread_mutex_lock(union pthread_mutex_t *__mutex);
int pthread_mutex_setprioceiling(union pthread_mutex_t *__mutex, int __prioceiling, int *__old_ceiling);
int pthread_mutex_timedlock(union pthread_mutex_t *__mutex, const struct timespec *__abstime);
int pthread_mutex_trylock(union pthread_mutex_t *__mutex);
int pthread_mutex_unlock(union pthread_mutex_t *__mutex);
int pthread_mutexattr_destroy(union pthread_mutexattr_t *__attr);
int pthread_mutexattr_getprioceiling(const union pthread_mutexattr_t *__attr, int *__prioceiling);
int pthread_mutexattr_getprotocol(const union pthread_mutexattr_t *__attr, int *__protocol);
int pthread_mutexattr_getpshared(const union pthread_mutexattr_t *__attr, int *__pshared);
int pthread_mutexattr_getrobust(union pthread_mutexattr_t *, int *);
int pthread_mutexattr_getrobust_np(union pthread_mutexattr_t *, int *);
int pthread_mutexattr_gettype(const union pthread_mutexattr_t *__attr, int *__kind);
int pthread_mutexattr_init(union pthread_mutexattr_t *__attr);
int pthread_mutexattr_setprioceiling(union pthread_mutexattr_t *__attr, int __prioceiling);
int pthread_mutexattr_setprotocol(union pthread_mutexattr_t *__attr, int __protocol);
int pthread_mutexattr_setpshared(union pthread_mutexattr_t *__attr, int __pshared);
int pthread_mutexattr_setrobust(union pthread_mutexattr_t *, int);
int pthread_mutexattr_setrobust_np(union pthread_mutexattr_t *, int);
int pthread_mutexattr_settype(union pthread_mutexattr_t *__attr, int __kind);
int pthread_once(pthread_once_t *__once_control, void (*__init_routine)(void));
int pthread_rwlock_clockrdlock(union pthread_rwlock_t *__rwlock, clockid_t __clockid, const struct timespec *__abstime);
int pthread_rwlock_clockwrlock(union pthread_rwlock_t *__rwlock, clockid_t __clockid, const struct timespec *__abstime);
int pthread_rwlock_destroy(union pthread_rwlock_t *__rwlock);
int pthread_rwlock_init(union pthread_rwlock_t *__rwlock, const union pthread_rwlockattr_t *__attr);
int pthread_rwlock_rdlock(union pthread_rwlock_t *__rwlock);
int pthread_rwlock_timedrdlock(union pthread_rwlock_t *__rwlock, const struct timespec *__abstime);
int pthread_rwlock_timedwrlock(union pthread_rwlock_t *__rwlock, const struct timespec *__abstime);
int pthread_rwlock_tryrdlock(union pthread_rwlock_t *__rwlock);
int pthread_rwlock_trywrlock(union pthread_rwlock_t *__rwlock);
int pthread_rwlock_unlock(union pthread_rwlock_t *__rwlock);
int pthread_rwlock_wrlock(union pthread_rwlock_t *__rwlock);
int pthread_rwlockattr_destroy(union pthread_rwlockattr_t *__attr);
int pthread_rwlockattr_getkind_np(const union pthread_rwlockattr_t *__attr, int *__pref);
int pthread_rwlockattr_getpshared(const union pthread_rwlockattr_t *__attr, int *__pshared);
int pthread_rwlockattr_init(union pthread_rwlockattr_t *__attr);
int pthread_rwlockattr_setkind_np(union pthread_rwlockattr_t *__attr, int __pref);
int pthread_rwlockattr_setpshared(union pthread_rwlockattr_t *__attr, int __pshared);
pthread_t pthread_self(void);
int pthread_setaffinity_np(pthread_t __th, size_t __cpusetsize, const struct cpu_set_t *__cpuset);
int pthread_setattr_default_np(const union pthread_attr_t *__attr);
int pthread_setcancelstate(int __state, int *__oldstate);
int pthread_setcanceltype(int __type, int *__oldtype);
int pthread_setconcurrency(int __level);
int pthread_setname_np(pthread_t __target_thread, const char *__name);
int pthread_setschedparam(pthread_t __target_thread, int __policy, const struct sched_param *__param);
int pthread_setschedprio(pthread_t __target_thread, int __prio);
int pthread_setspecific(pthread_key_t __key, const void *__pointer);
int pthread_sigmask(int __how, const struct __sigset_t *__newmask, struct __sigset_t *__oldmask);
int pthread_sigqueue(pthread_t __threadid, int __signo, const union sigval __value);
int pthread_spin_destroy(pthread_spinlock_t *__lock);
int pthread_spin_init(pthread_spinlock_t *__lock, int __pshared);
int pthread_spin_lock(pthread_spinlock_t *__lock);
int pthread_spin_trylock(pthread_spinlock_t *__lock);
int pthread_spin_unlock(pthread_spinlock_t *__lock);
void pthread_testcancel(void);
int pthread_timedjoin_np(pthread_t __th, void **__thread_return, const struct timespec *__abstime);
int pthread_tryjoin_np(pthread_t __th, void **__thread_return);
int pthread_yield(void);
long ptrace(enum __ptrace_request __request, ...);
char *ptsname(int __fd);
int ptsname_r(int __fd, char *__buf, size_t __buflen);
int putc(int __c, FILE *__stream);
int putc_unlocked(int __c, FILE *__stream);
int putchar(int __c);
int putchar_unlocked(int __c);
int putenv(char *__string);
int putgrent(const struct group *__p, FILE *__f);
int putpwent(const struct passwd *__p, FILE *__f);
int puts(const char *__s);
int putspent(const struct spwd *__p, FILE *__stream);
struct utmp *pututline(const struct utmp *__utmp_ptr);
int putw(int __w, FILE *__stream);
wint_t putwc(int __wc, __FILE *__stream);
wint_t putwc_unlocked(int __wc, __FILE *__stream);
wint_t putwchar(int __wc);
wint_t putwchar_unlocked(int __wc);
void *pvalloc(size_t __size);
ssize_t pwrite(int __fd, const void *__buf, size_t __n, __off_t __offset);
ssize_t pwrite64(int __fd, const void *__buf, size_t __n, __off64_t __offset);
ssize_t pwritev(int __fd, const struct iovec *__iovec, int __count, __off_t __offset);
ssize_t pwritev2(int __fd, const struct iovec *__iodev, int __count, __off_t __offset, int __flags);
ssize_t pwritev64(int __fd, const struct iovec *__iovec, int __count, __off64_t __offset);
ssize_t pwritev64v2(int __fd, const struct iovec *__iodev, int __count, __off64_t __offset, int __flags);
char *qecvt(long double __value, int __ndigit, int *__decpt, int *__sign);
int qecvt_r(long double __value, int __ndigit, int *__decpt, int *__sign, char *__buf, size_t __len);
char *qfcvt(long double __value, int __ndigit, int *__decpt, int *__sign);
int qfcvt_r(long double __value, int __ndigit, int *__decpt, int *__sign, char *__buf, size_t __len);
char *qgcvt(long double __value, int __ndigit, char *__buf);
void qsort(void *__base, size_t __nmemb, size_t __size, __compar_fn_t __compar);
void qsort_r(void *__base, size_t __nmemb, size_t __size, __compar_d_fn_t __compar, void *__arg);
_Noreturn void quick_exit(int __status);
int raise(int __sig);
int rand(void);
int rand_r(unsigned int *__seed);
long random(void);
int random_r(struct random_data *__buf, int *__result);
void *rawmemchr(const void *__s, int __c);
int rcmd(char **__ahost, unsigned short __rport, const char *__locuser, const char *__remuser, const char *__cmd, int *__fd2p);
int rcmd_af(char **__ahost, unsigned short __rport, const char *__locuser, const char *__remuser, const char *__cmd, int *__fd2p, sa_family_t __af);
int re_compile_fastmap(struct re_pattern_buffer *__buffer);
const char *re_compile_pattern(const char *__pattern, size_t __length, struct re_pattern_buffer *__buffer);
regoff_t re_match(struct re_pattern_buffer *__buffer, const char *__String, regoff_t __length, regoff_t __start, struct re_registers *__regs);
regoff_t re_match_2(struct re_pattern_buffer *__buffer, const char *__string1, regoff_t __length1, const char *__string2, regoff_t __length2, regoff_t __start, struct re_registers *__regs, regoff_t __stop);
regoff_t re_search(struct re_pattern_buffer *__buffer, const char *__String, regoff_t __length, regoff_t __start, regoff_t __range, struct re_registers *__regs);
regoff_t re_search_2(struct re_pattern_buffer *__buffer, const char *__string1, regoff_t __length1, const char *__string2, regoff_t __length2, regoff_t __start, regoff_t __range, struct re_registers *__regs, regoff_t __stop);
void re_set_registers(struct re_pattern_buffer *__buffer, struct re_registers *__regs, __re_size_t __num_regs, regoff_t *__starts, regoff_t *__ends);
reg_syntax_t re_set_syntax(reg_syntax_t __syntax);
ssize_t read(int __fd, void *__buf, size_t __nbytes);
__ssize_t readahead(int __fd, __off64_t __offset, size_t __count);
struct dirent *readdir(DIR *__dirp);
struct dirent64 *readdir64(DIR *__dirp);
int readdir64_r(DIR *__dirp, struct dirent64 *__entry, struct dirent64 **__result);
int readdir_r(DIR *__dirp, struct dirent *__entry, struct dirent **__result);
ssize_t readlink(const char *__path, char *__buf, size_t __len);
ssize_t readlinkat(int __fd, const char *__path, char *__buf, size_t __len);
ssize_t readv(int __fd, const struct iovec *__iovec, int __count);
void *realloc(void *__ptr, size_t __size);
void *reallocarray(void *__ptr, size_t __nmemb, size_t __size);
char *realpath(const char *__name, char *__resolved);
ssize_t recv(int __fd, void *__buf, size_t __n, int __flags);
ssize_t recvfrom(int __fd, void *__buf, size_t __n, int __flags, union __SOCKADDR_ARG __addr, socklen_t *__addr_len);
int recvmmsg(int __fd, struct mmsghdr *__vmessages, unsigned int __vlen, int __flags, struct timespec *__tmo);
ssize_t recvmsg(int __fd, struct msghdr *__message, int __flags);
int regcomp(regex_t *__preg, const char *__pattern, int __cflags);
size_t regerror(int __errcode, const regex_t *__preg, char *__errbuf, size_t __errbuf_size);
int regexec(const regex_t *__preg, const char *__String, size_t __nmatch, struct regmatch_t *__pmatch, int __eflags);
void regfree(regex_t *__preg);
double remainder(double __x, double __y);
float remainderf(float __x, float __y);
_Float32 remainderf32(_Float32 __x, _Float32 __y);
_Float32x remainderf32x(_Float32x __x, _Float32x __y);
_Float64 remainderf64(_Float64 __x, _Float64 __y);
_Float64x remainderf64x(_Float64x __x, _Float64x __y);
long double remainderl(long double __x, long double __y);
int remap_file_pages(void *__start, size_t __size, int __prot, size_t __pgoff, int __flags);
int remove(const char *__filename);
int removexattr(const char *__path, const char *__name);
void remque(void *__elem);
double remquo(double __x, double __y, int *__quo);
float remquof(float __x, float __y, int *__quo);
_Float32 remquof32(_Float32 __x, _Float32 __y, int *__quo);
_Float32x remquof32x(_Float32x __x, _Float32x __y, int *__quo);
_Float64 remquof64(_Float64 __x, _Float64 __y, int *__quo);
_Float64x remquof64x(_Float64x __x, _Float64x __y, int *__quo);
long double remquol(long double __x, long double __y, int *__quo);
int rename(const char *__old, const char *__new);
int renameat(int __oldfd, const char *__old, int __newfd, const char *__new);
int renameat2(int __oldfd, const char *__old, int __newfd, const char *__new, unsigned int __flags);
int revoke(const char *__file);
void rewind(FILE *__stream);
void rewinddir(DIR *__dirp);
int rexec(char **__ahost, int __rport, const char *__name, const char *__pass, const char *__cmd, int *__fd2p);
int rexec_af(char **__ahost, int __rport, const char *__name, const char *__pass, const char *__cmd, int *__fd2p, sa_family_t __af);
char *rindex(const char *__s, int __c);
double rint(double __x);
float rintf(float __x);
_Float32 rintf32(_Float32 __x);
_Float32x rintf32x(_Float32x __x);
_Float64 rintf64(_Float64 __x);
_Float64x rintf64x(_Float64x __x);
long double rintl(long double __x);
int rmdir(const char *__path);
double rootn(double __x, long long __y);
float rootnf(float __x, long long __y);
_Float32 rootnf32(_Float32 __x, long long __y);
_Float32x rootnf32x(_Float32x __x, long long __y);
_Float64 rootnf64(_Float64 __x, long long __y);
_Float64x rootnf64x(_Float64x __x, long long __y);
long double rootnl(long double __x, long long __y);
double round(double __x);
double roundeven(double __x);
float roundevenf(float __x);
_Float32 roundevenf32(_Float32 __x);
_Float32x roundevenf32x(_Float32x __x);
_Float64 roundevenf64(_Float64 __x);
_Float64x roundevenf64x(_Float64x __x);
long double roundevenl(long double __x);
float roundf(float __x);
_Float32 roundf32(_Float32 __x);
_Float32x roundf32x(_Float32x __x);
_Float64 roundf64(_Float64 __x);
_Float64x roundf64x(_Float64x __x);
long double roundl(long double __x);
int rpmatch(const char *__response);
int rresvport(int *__alport);
int rresvport_af(int *__alport, sa_family_t __af);
double rsqrt(double __x);
float rsqrtf(float __x);
_Float32 rsqrtf32(_Float32 __x);
_Float32x rsqrtf32x(_Float32x __x);
_Float64 rsqrtf64(_Float64 __x);
_Float64x rsqrtf64x(_Float64x __x);
long double rsqrtl(long double __x);
int ruserok(const char *__rhost, int __suser, const char *__remuser, const char *__locuser);
int ruserok_af(const char *__rhost, int __suser, const char *__remuser, const char *__locuser, sa_family_t __af);
void *sbrk(intptr_t __delta);
double scalb(double __x, double __n);
float scalbf(float __x, float __n);
long double scalbl(long double __x, long double __n);
double scalbln(double __x, long __n);
float scalblnf(float __x, long __n);
_Float32 scalblnf32(_Float32 __x, long __n);
_Float32x scalblnf32x(_Float32x __x, long __n);
_Float64 scalblnf64(_Float64 __x, long __n);
_Float64x scalblnf64x(_Float64x __x, long __n);
long double scalblnl(long double __x, long __n);
double scalbn(double __x, int __n);
float scalbnf(float __x, int __n);
_Float32 scalbnf32(_Float32 __x, int __n);
_Float32x scalbnf32x(_Float32x __x, int __n);
_Float64 scalbnf64(_Float64 __x, int __n);
_Float64x scalbnf64x(_Float64x __x, int __n);
long double scalbnl(long double __x, int __n);
int scandir(const char *__dir, struct dirent ***__namelist, int (*__selector)(const struct dirent *), int (*__cmp)(const struct dirent **, const struct dirent **));
int scandir64(const char *__dir, struct dirent64 ***__namelist, int (*__selector)(const struct dirent64 *), int (*__cmp)(const struct dirent64 **, const struct dirent64 **));
int scandirat(int __dfd, const char *__dir, struct dirent ***__namelist, int (*__selector)(const struct dirent *), int (*__cmp)(const struct dirent **, const struct dirent **));
int scandirat64(int __dfd, const char *__dir, struct dirent64 ***__namelist, int (*__selector)(const struct dirent64 *), int (*__cmp)(const struct dirent64 **, const struct dirent64 **));
int scanf(const char *__format, ...);
int sched_get_priority_max(int __algorithm);
int sched_get_priority_min(int __algorithm);
int sched_getaffinity(__pid_t __pid, size_t __cpusetsize, struct cpu_set_t *__cpuset);
int sched_getattr(pid_t tid, struct sched_attr *attr, unsigned int size, unsigned int flags);
int sched_getcpu(void);
int sched_getparam(__pid_t __pid, struct sched_param *__param);
int sched_getscheduler(__pid_t __pid);
int sched_rr_get_interval(__pid_t __pid, struct timespec *__t);
int sched_setaffinity(__pid_t __pid, size_t __cpusetsize, const struct cpu_set_t *__cpuset);
int sched_setattr(pid_t tid, struct sched_attr *attr, unsigned int flags);
int sched_setparam(__pid_t __pid, const struct sched_param *__param);
int sched_setscheduler(__pid_t __pid, int __policy, const struct sched_param *__param);
int sched_yield(void);
char *secure_getenv(const char *__name);
unsigned short *seed48(unsigned short *__seed16v);
int seed48_r(unsigned short *__seed16v, struct drand48_data *__buffer);
void seekdir(DIR *__dirp, long __pos);
int select(int __nfds, struct fd_set *__readfds, struct fd_set *__writefds, struct fd_set *__exceptfds, struct timeval *__timeout);
int sem_clockwait(union sem_t *__sem, clockid_t clock, const struct timespec *__abstime);
int sem_close(union sem_t *__sem);
int sem_destroy(union sem_t *__sem);
int sem_getvalue(union sem_t *__sem, int *__sval);
int sem_init(union sem_t *__sem, int __pshared, unsigned int __value);
union sem_t *sem_open(const char *__name, int __oflag, ...);
int sem_post(union sem_t *__sem);
int sem_timedwait(union sem_t *__sem, const struct timespec *__abstime);
int sem_trywait(union sem_t *__sem);
int sem_unlink(const char *__name);
int sem_wait(union sem_t *__sem);
int semctl(int __semid, int __semnum, int __cmd, ...);
int semget(key_t __key, int __nsems, int __semflg);
int semop(int __semid, struct sembuf *__sops, size_t __nsops);
int semtimedop(int __semid, struct sembuf *__sops, size_t __nsops, const struct timespec *__timeout);
ssize_t send(int __fd, const void *__buf, size_t __n, int __flags);
ssize_t sendfile(int __out_fd, int __in_fd, off_t *__offset, size_t __count);
ssize_t sendfile64(int __out_fd, int __in_fd, __off64_t *__offset, size_t __count);
int sendmmsg(int __fd, struct mmsghdr *__vmessages, unsigned int __vlen, int __flags);
ssize_t sendmsg(int __fd, const struct msghdr *__message, int __flags);
ssize_t sendto(int __fd, const void *__buf, size_t __n, int __flags, union __CONST_SOCKADDR_ARG __addr, socklen_t __addr_len);
void setbuf(FILE *__stream, char *__buf);
void setbuffer(FILE *__stream, char *__buf, size_t __size);
int setcontext(const struct ucontext_t *__ucp);
int setdomainname(const char *__name, size_t __len);
int setegid(__gid_t __gid);
int setenv(const char *__name, const char *__value, int __replace);
int seteuid(__uid_t __uid);
int setgid(__gid_t __gid);
void setgrent(void);
int setgroups(size_t __n, const __gid_t *__groups);
void sethostent(int __stay_open);
int sethostid(long __id);
int sethostname(const char *__name, size_t __len);
int setipv4sourcefilter(int __s, struct in_addr __interface_addr, struct in_addr __group, unsigned int __fmode, unsigned int __numsrc, const struct in_addr *__slist);
int setitimer(__itimer_which_t __which, const struct itimerval *__new, struct itimerval *__old);
int setjmp(jmp_buf __env);
void setlinebuf(FILE *__stream);
char *setlocale(int __category, const char *__locale);
int setlogin(const char *__name);
int setlogmask(int __mask);
void setnetent(int __stay_open);
int setnetgrent(const char *__netgroup);
int setns(int __fd, int __nstype);
int setpayload(double *__x, double __payload);
int setpayloadf(float *__x, float __payload);
int setpayloadf32(_Float32 *__x, _Float32 __payload);
int setpayloadf32x(_Float32x *__x, _Float32x __payload);
int setpayloadf64(_Float64 *__x, _Float64 __payload);
int setpayloadf64x(_Float64x *__x, _Float64x __payload);
int setpayloadl(long double *__x, long double __payload);
int setpayloadsig(double *__x, double __payload);
int setpayloadsigf(float *__x, float __payload);
int setpayloadsigf32(_Float32 *__x, _Float32 __payload);
int setpayloadsigf32x(_Float32x *__x, _Float32x __payload);
int setpayloadsigf64(_Float64 *__x, _Float64 __payload);
int setpayloadsigf64x(_Float64x *__x, _Float64x __payload);
int setpayloadsigl(long double *__x, long double __payload);
int setpgid(__pid_t __pid, __pid_t __pgid);
int setpgrp(void);
int setpriority(__priority_which_t __which, id_t __who, int __prio);
void setprotoent(int __stay_open);
void setpwent(void);
int setregid(__gid_t __rgid, __gid_t __egid);
int setresgid(__gid_t __rgid, __gid_t __egid, __gid_t __sgid);
int setresuid(__uid_t __ruid, __uid_t __euid, __uid_t __suid);
int setreuid(__uid_t __ruid, __uid_t __euid);
int setrlimit(__rlimit_resource_t __resource, const struct rlimit *__rlimits);
int setrlimit64(__rlimit_resource_t __resource, const struct rlimit64 *__rlimits);
void setrpcent(int __stayopen);
void setservent(int __stay_open);
__pid_t setsid(void);
int setsockopt(int __fd, int __level, int __optname, const void *__optval, socklen_t __optlen);
int setsourcefilter(int __s, unsigned int __interface_addr, const struct sockaddr *__group, socklen_t __grouplen, unsigned int __fmode, unsigned int __numsrc, const struct sockaddr_storage *__slist);
void setspent(void);
char *setstate(char *__statebuf);
int setstate_r(char *__statebuf, struct random_data *__buf);
int settimeofday(const struct timeval *__tv, const struct timezone *__tz);
int setuid(__uid_t __uid);
void setusershell(void);
void setutent(void);
int setvbuf(FILE *__stream, char *__buf, int __modes, size_t __n);
int setxattr(const char *__path, const char *__name, const void *__value, size_t __size, int __flags);
struct spwd *sgetspent(const char *__string);
int sgetspent_r(const char *__string, struct spwd *__result_buf, char *__buffer, size_t __buflen, struct spwd **__result);
int shm_open(const char *__name, int __oflag, mode_t __mode);
int shm_unlink(const char *__name);
void *shmat(int __shmid, const void *__shmaddr, int __shmflg);
int shmctl(int __shmid, int __cmd, struct shmid_ds *__buf);
int shmdt(const void *__shmaddr);
int shmget(key_t __key, size_t __size, int __shmflg);
int shutdown(int __fd, int __how);
const char *sigabbrev_np(int __sig);
int sigaction(int __sig, const struct sigaction *__act, struct sigaction *__oact);
int sigaddset(sigset_t *__set, int __signo);
int sigaltstack(const struct stack_t *__ss, struct stack_t *__oss);
int sigandset(sigset_t *__set, const sigset_t *__left, const sigset_t *__right);
int sigblock(int __mask);
int sigdelset(sigset_t *__set, int __signo);
const char *sigdescr_np(int __sig);
int sigemptyset(sigset_t *__set);
int sigfillset(sigset_t *__set);
int siggetmask(void);
int sighold(int __sig);
int sigignore(int __sig);
int siginterrupt(int __sig, int __interrupt);
int sigisemptyset(const sigset_t *__set);
int sigismember(const sigset_t *__set, int __signo);
_Noreturn void siglongjmp(sigjmp_buf __env, int __val);
__sighandler_t signal(int __sig, __sighandler_t __handler);
int signalfd(int __fd, const sigset_t *__mask, int __flags);
double significand(double __x);
float significandf(float __x);
long double significandl(long double __x);
int sigorset(sigset_t *__set, const sigset_t *__left, const sigset_t *__right);
int sigpause(int __sig);
int sigpending(sigset_t *__set);
int sigprocmask(int __how, const sigset_t *__set, sigset_t *__oset);
int sigqueue(__pid_t __pid, int __sig, const union sigval __val);
int sigrelse(int __sig);
int sigreturn(struct sigcontext *__scp);
__sighandler_t sigset(int __sig, __sighandler_t __disp);
int sigsetmask(int __mask);
int sigstack(struct sigstack *__ss, struct sigstack *__oss);
int sigsuspend(const sigset_t *__set);
int sigtimedwait(const sigset_t *__set, struct siginfo_t *__info, const struct timespec *__timeout);
int sigwait(const sigset_t *__set, int *__sig);
int sigwaitinfo(const sigset_t *__set, struct siginfo_t *__info);
double sin(double __x);
void sincos(double __x, double *__sinx, double *__cosx);
void sincosf(float __x, float *__sinx, float *__cosx);
void sincosf32(_Float32 __x, _Float32 *__sinx, _Float32 *__cosx);
void sincosf32x(_Float32x __x, _Float32x *__sinx, _Float32x *__cosx);
void sincosf64(_Float64 __x, _Float64 *__sinx, _Float64 *__cosx);
void sincosf64x(_Float64x __x, _Float64x *__sinx, _Float64x *__cosx);
void sincosl(long double __x, long double *__sinx, long double *__cosx);
float sinf(float __x);
_Float32 sinf32(_Float32 __x);
_Float32x sinf32x(_Float32x __x);
_Float64 sinf64(_Float64 __x);
_Float64x sinf64x(_Float64x __x);
double sinh(double __x);
float sinhf(float __x);
_Float32 sinhf32(_Float32 __x);
_Float32x sinhf32x(_Float32x __x);
_Float64 sinhf64(_Float64 __x);
_Float64x sinhf64x(_Float64x __x);
long double sinhl(long double __x);
long double sinl(long double __x);
double sinpi(double __x);
float sinpif(float __x);
_Float32 sinpif32(_Float32 __x);
_Float32x sinpif32x(_Float32x __x);
_Float64 sinpif64(_Float64 __x);
_Float64x sinpif64x(_Float64x __x);
long double sinpil(long double __x);
unsigned int sleep(unsigned int __seconds);
int snprintf(char *__s, size_t __maxlen, const char *__format, ...);
int sockatmark(int __fd);
int socket(int __domain, int __type, int __protocol);
int socketpair(int __domain, int __type, int __protocol, int *__fds);
__ssize_t splice(int __fdin, __off64_t *__offin, int __fdout, __off64_t *__offout, size_t __len, unsigned int __flags);
int sprintf(char *__s, const char *__format, ...);
double sqrt(double __x);
float sqrtf(float __x);
_Float32 sqrtf32(_Float32 __x);
_Float32x sqrtf32x(_Float32x __x);
_Float64 sqrtf64(_Float64 __x);
_Float64x sqrtf64x(_Float64x __x);
long double sqrtl(long double __x);
void srand(unsigned int __seed);
void srand48(long __seedval);
int srand48_r(long __seedval, struct drand48_data *__buffer);
void srandom(unsigned int __seed);
int srandom_r(unsigned int __seed, struct random_data *__buf);
int sscanf(const char *__s, const char *__format, ...);
__sighandler_t ssignal(int __sig, __sighandler_t __handler);
int stat(const char *__file, struct stat *__buf);
int stat64(const char *__file, struct stat64 *__buf);
int statvfs(const char *__file, struct statvfs *__buf);
int statvfs64(const char *__file, struct statvfs64 *__buf);
int statx(int __dirfd, const char *__path, int __flags, unsigned int __mask, struct statx *__buf);
char *stpcpy(char *__dest, const char *__src);
char *stpncpy(char *__dest, const char *__src, size_t __n);
int strcasecmp(const char *__s1, const char *__s2);
int strcasecmp_l(const char *__s1, const char *__s2, locale_t __loc);
char *strcasestr(const char *__haystack, const char *__needle);
char *strcat(char *__dest, const char *__src);
char *strchr(const char *__s, int __c);
char *strchrnul(const char *__s, int __c);
int strcmp(const char *__s1, const char *__s2);
int strcoll(const char *__s1, const char *__s2);
int strcoll_l(const char *__s1, const char *__s2, locale_t __l);
char *strcpy(char *__dest, const char *__src);
size_t strcspn(const char *__s, const char *__reject);
char *strdup(const char *__s);
char *strerror(int __errnum);
char *strerror_l(int __errnum, locale_t __l);
char *strerror_r(int __errnum, char *__buf, size_t __buflen);
const char *strerrordesc_np(int __err);
const char *strerrorname_np(int __err);
int strfromd(char *__dest, size_t __size, const char *__format, double __f);
int strfromf(char *__dest, size_t __size, const char *__format, float __f);
int strfromf32(char *__dest, size_t __size, const char *__format, _Float32 __f);
int strfromf32x(char *__dest, size_t __size, const char *__format, _Float32x __f);
int strfromf64(char *__dest, size_t __size, const char *__format, _Float64 __f);
int strfromf64x(char *__dest, size_t __size, const char *__format, _Float64x __f);
int strfroml(char *__dest, size_t __size, const char *__format, long double __f);
char *strfry(char *__string);
size_t strftime(char *__s, size_t __maxsize, const char *__format, const struct tm *__tp);
size_t strftime_l(char *__s, size_t __maxsize, const char *__format, const struct tm *__tp, locale_t __loc);
size_t strlcat(char *__dest, const char *__src, size_t __n);
size_t strlcpy(char *__dest, const char *__src, size_t __n);
size_t strlen(const char *__s);
int strncasecmp(const char *__s1, const char *__s2, size_t __n);
int strncasecmp_l(const char *__s1, const char *__s2, size_t __n, locale_t __loc);
char *strncat(char *__dest, const char *__src, size_t __n);
int strncmp(const char *__s1, const char *__s2, size_t __n);
char *strncpy(char *__dest, const char *__src, size_t __n);
char *strndup(const char *__string, size_t __n);
size_t strnlen(const char *__string, size_t __maxlen);
char *strpbrk(const char *__s, const char *__accept);
char *strptime(const char *__s, const char *__fmt, struct tm *__tp);
char *strptime_l(const char *__s, const char *__fmt, struct tm *__tp, locale_t __loc);
char *strrchr(const char *__s, int __c);
char *strsep(char **__stringp, const char *__delim);
char *strsignal(int __sig);
size_t strspn(const char *__s, const char *__accept);
char *strstr(const char *__haystack, const char *__needle);
double strtod(const char *__nptr, char **__endptr);
double strtod_l(const char *__nptr, char **__endptr, locale_t __loc);
float strtof(const char *__nptr, char **__endptr);
_Float32 strtof32(const char *__nptr, char **__endptr);
_Float32 strtof32_l(const char *__nptr, char **__endptr, locale_t __loc);
_Float32x strtof32x(const char *__nptr, char **__endptr);
_Float32x strtof32x_l(const char *__nptr, char **__endptr, locale_t __loc);
_Float64 strtof64(const char *__nptr, char **__endptr);
_Float64 strtof64_l(const char *__nptr, char **__endptr, locale_t __loc);
_Float64x strtof64x(const char *__nptr, char **__endptr);
_Float64x strtof64x_l(const char *__nptr, char **__endptr, locale_t __loc);
float strtof_l(const char *__nptr, char **__endptr, locale_t __loc);
intmax_t strtoimax(const char *__nptr, char **__endptr, int __base);
char *strtok(char *__s, const char *__delim);
char *strtok_r(char *__s, const char *__delim, char **__save_ptr);
long strtol(const char *__nptr, char **__endptr, int __base);
long strtol_l(const char *__nptr, char **__endptr, int __base, locale_t __loc);
long double strtold(const char *__nptr, char **__endptr);
long double strtold_l(const char *__nptr, char **__endptr, locale_t __loc);
long long strtoll(const char *__nptr, char **__endptr, int __base);
long long strtoll_l(const char *__nptr, char **__endptr, int __base, locale_t __loc);
long long strtoq(const char *__nptr, char **__endptr, int __base);
unsigned long strtoul(const char *__nptr, char **__endptr, int __base);
unsigned long strtoul_l(const char *__nptr, char **__endptr, int __base, locale_t __loc);
unsigned long long strtoull(const char *__nptr, char **__endptr, int __base);
unsigned long long strtoull_l(const char *__nptr, char **__endptr, int __base, locale_t __loc);
uintmax_t strtoumax(const char *__nptr, char **__endptr, int __base);
unsigned long long strtouq(const char *__nptr, char **__endptr, int __base);
int strverscmp(const char *__s1, const char *__s2);
size_t strxfrm(char *__dest, const char *__src, size_t __n);
size_t strxfrm_l(char *__dest, const char *__src, size_t __n, locale_t __l);
void swab(const void *__from, void *__to, ssize_t __n);
int swapcontext(struct ucontext_t *__oucp, const struct ucontext_t *__ucp);
int swprintf(int *__s, size_t __n, const int *__format, ...);
int swscanf(const int *__s, const int *__format, ...);
int symlink(const char *__from, const char *__to);
int symlinkat(const char *__from, int __tofd, const char *__to);
void sync(void);
int sync_file_range(int __fd, __off64_t __offset, __off64_t __count, unsigned int __flags);
int syncfs(int __fd);
long syscall(long __sysno, ...);
long sysconf(int __name);
int sysinfo(struct sysinfo *__info);
void syslog(int __pri, const char *__fmt, ...);
int system(const char *__command);
__sighandler_t sysv_signal(int __sig, __sighandler_t __handler);
double tan(double __x);
float tanf(float __x);
_Float32 tanf32(_Float32 __x);
_Float32x tanf32x(_Float32x __x);
_Float64 tanf64(_Float64 __x);
_Float64x tanf64x(_Float64x __x);
double tanh(double __x);
float tanhf(float __x);
_Float32 tanhf32(_Float32 __x);
_Float32x tanhf32x(_Float32x __x);
_Float64 tanhf64(_Float64 __x);
_Float64x tanhf64x(_Float64x __x);
long double tanhl(long double __x);
long double tanl(long double __x);
double tanpi(double __x);
float tanpif(float __x);
_Float32 tanpif32(_Float32 __x);
_Float32x tanpif32x(_Float32x __x);
_Float64 tanpif64(_Float64 __x);
_Float64x tanpif64x(_Float64x __x);
long double tanpil(long double __x);
int tcdrain(int __fd);
int tcflow(int __fd, int __action);
int tcflush(int __fd, int __queue_selector);
int tcgetattr(int __fd, struct termios *__termios_p);
__pid_t tcgetpgrp(int __fd);
__pid_t tcgetsid(int __fd);
int tcsendbreak(int __fd, int __duration);
int tcsetattr(int __fd, int __optional_actions, const struct termios *__termios_p);
int tcsetpgrp(int __fd, __pid_t __pgrp_id);
void *tdelete(const void *__key, void **__rootp, __compar_fn_t __compar);
void tdestroy(void *__root, __free_fn_t __freefct);
__ssize_t tee(int __fdin, int __fdout, size_t __len, unsigned int __flags);
long telldir(DIR *__dirp);
char *tempnam(const char *__dir, const char *__pfx);
void *tfind(const void *__key, void *const *__rootp, __compar_fn_t __compar);
double tgamma(double);
float tgammaf(float);
_Float32 tgammaf32(_Float32);
_Float32x tgammaf32x(_Float32x);
_Float64 tgammaf64(_Float64);
_Float64x tgammaf64x(_Float64x);
long double tgammal(long double);
int tgkill(__pid_t __tgid, __pid_t __tid, int __signal);
time_t time(time_t *__timer);
time_t timegm(struct tm *__tp);
time_t timelocal(struct tm *__tp);
int timer_create(clockid_t __clock_id, struct sigevent *__evp, timer_t *__timerid);
int timer_delete(timer_t __timerid);
int timer_getoverrun(timer_t __timerid);
int timer_gettime(timer_t __timerid, struct itimerspec *__value);
int timer_settime(timer_t __timerid, int __flags, const struct itimerspec *__value, struct itimerspec *__ovalue);
int timerfd_create(__clockid_t __clock_id, int __flags);
int timerfd_gettime(int __ufd, struct itimerspec *__otmr);
int timerfd_settime(int __ufd, int __flags, const struct itimerspec *__utmr, struct itimerspec *__otmr);
clock_t times(struct tms *__buffer);
int timespec_get(struct timespec *__ts, int __base);
int timespec_getres(struct timespec *__ts, int __base);
FILE *tmpfile(void);
FILE *tmpfile64(void);
char *tmpnam(char *);
char *tmpnam_r(char *__s);
int toascii(int __c);
int tolower(int __c);
int tolower_l(int __c, locale_t __l);
int totalorder(const double *__x, const double *__y);
int totalorderf(const float *__x, const float *__y);
int totalorderf32(const _Float32 *__x, const _Float32 *__y);
int totalorderf32x(const _Float32x *__x, const _Float32x *__y);
int totalorderf64(const _Float64 *__x, const _Float64 *__y);
int totalorderf64x(const _Float64x *__x, const _Float64x *__y);
int totalorderl(const long double *__x, const long double *__y);
int totalordermag(const double *__x, const double *__y);
int totalordermagf(const float *__x, const float *__y);
int totalordermagf32(const _Float32 *__x, const _Float32 *__y);
int totalordermagf32x(const _Float32x *__x, const _Float32x *__y);
int totalordermagf64(const _Float64 *__x, const _Float64 *__y);
int totalordermagf64x(const _Float64x *__x, const _Float64x *__y);
int totalordermagl(const long double *__x, const long double *__y);
int toupper(int __c);
int toupper_l(int __c, locale_t __l);
wint_t towctrans(wint_t __wc, wctrans_t __desc);
wint_t towctrans_l(wint_t __wc, wctrans_t __desc, locale_t __locale);
wint_t towlower(wint_t __wc);
wint_t towlower_l(wint_t __wc, locale_t __locale);
wint_t towupper(wint_t __wc);
wint_t towupper_l(wint_t __wc, locale_t __locale);
double trunc(double __x);
int truncate(const char *__file, __off_t __length);
int truncate64(const char *__file, __off64_t __length);
float truncf(float __x);
_Float32 truncf32(_Float32 __x);
_Float32x truncf32x(_Float32x __x);
_Float64 truncf64(_Float64 __x);
_Float64x truncf64x(_Float64x __x);
long double truncl(long double __x);
void *tsearch(const void *__key, void **__rootp, __compar_fn_t __compar);
char *ttyname(int __fd);
int ttyname_r(int __fd, char *__buf, size_t __buflen);
int ttyslot(void);
void twalk(const void *__root, __action_fn_t __action);
void twalk_r(const void *__root, void (*)(const void *__nodep, enum VISIT __value, void *__closure), void *__closure);
void tzset(void);
unsigned int uabs(int __x);
__useconds_t ualarm(__useconds_t __value, __useconds_t __interval);
double ufromfp(double __x, int __round, unsigned int __width);
float ufromfpf(float __x, int __round, unsigned int __width);
_Float32 ufromfpf32(_Float32 __x, int __round, unsigned int __width);
_Float32x ufromfpf32x(_Float32x __x, int __round, unsigned int __width);
_Float64 ufromfpf64(_Float64 __x, int __round, unsigned int __width);
_Float64x ufromfpf64x(_Float64x __x, int __round, unsigned int __width);
long double ufromfpl(long double __x, int __round, unsigned int __width);
double ufromfpx(double __x, int __round, unsigned int __width);
float ufromfpxf(float __x, int __round, unsigned int __width);
_Float32 ufromfpxf32(_Float32 __x, int __round, unsigned int __width);
_Float32x ufromfpxf32x(_Float32x __x, int __round, unsigned int __width);
_Float64 ufromfpxf64(_Float64 __x, int __round, unsigned int __width);
_Float64x ufromfpxf64x(_Float64x __x, int __round, unsigned int __width);
long double ufromfpxl(long double __x, int __round, unsigned int __width);
unsigned long ulabs(long __x);
int ulckpwdf(void);
unsigned long long ullabs(long long __x);
__mode_t umask(__mode_t __mask);
uintmax_t umaxabs(intmax_t __n);
int uname(struct utsname *__name);
int ungetc(int __c, FILE *__stream);
wint_t ungetwc(wint_t __wc, __FILE *__stream);
int unlink(const char *__name);
int unlinkat(int __fd, const char *__name, int __flag);
int unlockpt(int __fd);
int unsetenv(const char *__name);
int unshare(int __flags);
void updwtmp(const char *__wtmp_file, const struct utmp *__utmp);
locale_t uselocale(locale_t __dataset);
int usleep(__useconds_t __useconds);
int utime(const char *__file, const struct utimbuf *__file_times);
int utimensat(int __fd, const char *__path, const struct timespec *__times, int __flags);
int utimes(const char *__file, const struct timeval *__tvp);
int utmpname(const char *__file);
void *valloc(size_t __size);
int vasprintf(char **__ptr, const char *__f, char *__arg);
int vdprintf(int __fd, const char *__fmt, char *__arg);
_Noreturn void verr(int __status, const char *__format, char *);
_Noreturn void verrx(int __status, const char *, char *);
int versionsort(const struct dirent **__e1, const struct dirent **__e2);
int versionsort64(const struct dirent64 **__e1, const struct dirent64 **__e2);
__pid_t vfork(void);
int vfprintf(FILE *__s, const char *__format, char *__arg);
int vfscanf(FILE *__s, const char *__format, char *__arg);
int vfwprintf(__FILE *__s, const int *__format, char *__arg);
int vfwscanf(__FILE *__s, const int *__format, char *__arg);
int vhangup(void);
__ssize_t vmsplice(int __fdout, const struct iovec *__iov, size_t __count, unsigned int __flags);
int vprintf(const char *__format, char *__arg);
int vscanf(const char *__format, char *__arg);
int vsnprintf(char *__s, size_t __maxlen, const char *__format, char *__arg);
int vsprintf(char *__s, const char *__format, char *__arg);
int vsscanf(const char *__s, const char *__format, char *__arg);
int vswprintf(int *__s, size_t __n, const int *__format, char *__arg);
int vswscanf(const int *__s, const int *__format, char *__arg);
void vsyslog(int __pri, const char *__fmt, char *__ap);
void vwarn(const char *__format, char *);
void vwarnx(const char *__format, char *);
int vwprintf(const int *__format, char *__arg);
int vwscanf(const int *__format, char *__arg);
__pid_t wait(int *__stat_loc);
__pid_t wait3(int *__stat_loc, int __options, struct rusage *__usage);
__pid_t wait4(__pid_t __pid, int *__stat_loc, int __options, struct rusage *__usage);
int waitid(enum idtype_t __idtype, __id_t __id, struct siginfo_t *__infop, int __options);
__pid_t waitpid(__pid_t __pid, int *__stat_loc, int __options);
void warn(const char *__format, ...);
void warnx(const char *__format, ...);
int *wcpcpy(int *__dest, const int *__src);
int *wcpncpy(int *__dest, const int *__src, size_t __n);
size_t wcrtomb(char *__s, int __wc, mbstate_t *__ps);
int wcscasecmp(const int *__s1, const int *__s2);
int wcscasecmp_l(const int *__s1, const int *__s2, locale_t __loc);
int *wcscat(int *__dest, const int *__src);
int *wcschr(const int *__wcs, int __wc);
int *wcschrnul(const int *__s, int __wc);
int wcscmp(const int *__s1, const int *__s2);
int wcscoll(const int *__s1, const int *__s2);
int wcscoll_l(const int *__s1, const int *__s2, locale_t __loc);
int *wcscpy(int *__dest, const int *__src);
size_t wcscspn(const int *__wcs, const int *__reject);
int *wcsdup(const int *__s);
size_t wcsftime(int *__s, size_t __maxsize, const int *__format, const struct tm *__tp);
size_t wcsftime_l(int *__s, size_t __maxsize, const int *__format, const struct tm *__tp, locale_t __loc);
size_t wcslcat(int *__dest, const int *__src, size_t __n);
size_t wcslcpy(int *__dest, const int *__src, size_t __n);
size_t wcslen(const int *__s);
int wcsncasecmp(const int *__s1, const int *__s2, size_t __n);
int wcsncasecmp_l(const int *__s1, const int *__s2, size_t __n, locale_t __loc);
int *wcsncat(int *__dest, const int *__src, size_t __n);
int wcsncmp(const int *__s1, const int *__s2, size_t __n);
int *wcsncpy(int *__dest, const int *__src, size_t __n);
size_t wcsnlen(const int *__s, size_t __maxlen);
size_t wcsnrtombs(char *__dst, const int **__src, size_t __nwc, size_t __len, mbstate_t *__ps);
int *wcspbrk(const int *__wcs, const int *__accept);
int *wcsrchr(const int *__wcs, int __wc);
size_t wcsrtombs(char *__dst, const int **__src, size_t __len, mbstate_t *__ps);
size_t wcsspn(const int *__wcs, const int *__accept);
int *wcsstr(const int *__haystack, const int *__needle);
double wcstod(const int *__nptr, int **__endptr);
double wcstod_l(const int *__nptr, int **__endptr, locale_t __loc);
float wcstof(const int *__nptr, int **__endptr);
_Float32 wcstof32(const int *__nptr, int **__endptr);
_Float32 wcstof32_l(const int *__nptr, int **__endptr, locale_t __loc);
_Float32x wcstof32x(const int *__nptr, int **__endptr);
_Float32x wcstof32x_l(const int *__nptr, int **__endptr, locale_t __loc);
_Float64 wcstof64(const int *__nptr, int **__endptr);
_Float64 wcstof64_l(const int *__nptr, int **__endptr, locale_t __loc);
_Float64x wcstof64x(const int *__nptr, int **__endptr);
_Float64x wcstof64x_l(const int *__nptr, int **__endptr, locale_t __loc);
float wcstof_l(const int *__nptr, int **__endptr, locale_t __loc);
intmax_t wcstoimax(const __gwchar_t *__nptr, __gwchar_t **__endptr, int __base);
int *wcstok(int *__s, const int *__delim, int **__ptr);
long wcstol(const int *__nptr, int **__endptr, int __base);
long wcstol_l(const int *__nptr, int **__endptr, int __base, locale_t __loc);
long double wcstold(const int *__nptr, int **__endptr);
long double wcstold_l(const int *__nptr, int **__endptr, locale_t __loc);
long long wcstoll(const int *__nptr, int **__endptr, int __base);
long long wcstoll_l(const int *__nptr, int **__endptr, int __base, locale_t __loc);
size_t wcstombs(char *__s, const int *__pwcs, size_t __n);
long long wcstoq(const int *__nptr, int **__endptr, int __base);
unsigned long wcstoul(const int *__nptr, int **__endptr, int __base);
unsigned long wcstoul_l(const int *__nptr, int **__endptr, int __base, locale_t __loc);
unsigned long long wcstoull(const int *__nptr, int **__endptr, int __base);
unsigned long long wcstoull_l(const int *__nptr, int **__endptr, int __base, locale_t __loc);
uintmax_t wcstoumax(const __gwchar_t *__nptr, __gwchar_t **__endptr, int __base);
unsigned long long wcstouq(const int *__nptr, int **__endptr, int __base);
int *wcswcs(const int *__haystack, const int *__needle);
int wcswidth(const int *__s, size_t __n);
size_t wcsxfrm(int *__s1, const int *__s2, size_t __n);
size_t wcsxfrm_l(int *__s1, const int *__s2, size_t __n, locale_t __loc);
int wctob(wint_t __c);
int wctomb(char *__s, int __wchar);
wctrans_t wctrans(const char *__property);
wctrans_t wctrans_l(const char *__property, locale_t __locale);
wctype_t wctype(const char *__property);
wctype_t wctype_l(const char *__property, locale_t __locale);
int wcwidth(int __c);
int *wmemchr(const int *__s, int __c, size_t __n);
int wmemcmp(const int *__s1, const int *__s2, size_t __n);
int *wmemcpy(int *__s1, const int *__s2, size_t __n);
int *wmemmove(int *__s1, const int *__s2, size_t __n);
int *wmempcpy(int *__s1, const int *__s2, size_t __n);
int *wmemset(int *__s, int __c, size_t __n);
int wordexp(const char *__words, struct wordexp_t *__pwordexp, int __flags);
void wordfree(struct wordexp_t *__wordexp);
int wprintf(const int *__format, ...);
ssize_t write(int __fd, const void *__buf, size_t __n);
ssize_t writev(int __fd, const struct iovec *__iovec, int __count);
int wscanf(const int *__format, ...);
double y0(double);
float y0f(float);
_Float32 y0f32(_Float32);
_Float32x y0f32x(_Float32x);
_Float64 y0f64(_Float64);
_Float64x y0f64x(_Float64x);
long double y0l(long double);
double y1(double);
float y1f(float);
_Float32 y1f32(_Float32);
_Float32x y1f32x(_Float32x);
_Float64 y1f64(_Float64);
_Float64x y1f64x(_Float64x);
long double y1l(long double);
double yn(int, double);
float ynf(int, float);
_Float32 ynf32(int, _Float32);
_Float32x ynf32x(int, _Float32x);
_Float64 ynf64(int, _Float64);
_Float64x ynf64x(int, _Float64x);
long double ynl(int, long double);
