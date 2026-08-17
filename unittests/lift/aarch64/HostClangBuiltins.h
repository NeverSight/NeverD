// Stubs for NeverD LLVM-fork builtins when HighC syntax checks fall back
// to host clang (CI prebuilt LLVM packages do not ship clang).
#ifndef NEVERD_UNITTESTS_LIFT_AARCH64_HOSTCLANGBUILTINS_H
#define NEVERD_UNITTESTS_LIFT_AARCH64_HOSTCLANGBUILTINS_H

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

#if !__has_builtin(__builtin_arm_scvtf_fixed)
#define __builtin_arm_scvtf_fixed(...) ((unsigned)0)
#endif
#if !__has_builtin(__builtin_arm_ucvtf_fixed)
#define __builtin_arm_ucvtf_fixed(...) ((unsigned)0)
#endif
#if !__has_builtin(__builtin_arm_fcvtzs_fixed)
#define __builtin_arm_fcvtzs_fixed(...) ((unsigned)0)
#endif
#if !__has_builtin(__builtin_arm_fcvtzu_fixed)
#define __builtin_arm_fcvtzu_fixed(...) ((unsigned)0)
#endif

#if !__has_builtin(__builtin_arm_rcwcasp)
#define __builtin_arm_rcwcasp(...) ((unsigned __int128)0)
#define __builtin_arm_rcwcaspa(...) ((unsigned __int128)0)
#define __builtin_arm_rcwcaspal(...) ((unsigned __int128)0)
#define __builtin_arm_rcwcaspl(...) ((unsigned __int128)0)
#define __builtin_arm_rcwscasp(...) ((unsigned __int128)0)
#define __builtin_arm_rcwscaspa(...) ((unsigned __int128)0)
#define __builtin_arm_rcwscaspal(...) ((unsigned __int128)0)
#define __builtin_arm_rcwscaspl(...) ((unsigned __int128)0)
#endif

#if !__has_builtin(__builtin_arm_ldclr)
#define __builtin_arm_ldclr(...) ((unsigned long long)0)
#define __builtin_arm_ldeor(...) ((unsigned long long)0)
#define __builtin_arm_ldset(...) ((unsigned long long)0)
#define __builtin_arm_ldsmax(...) ((unsigned long long)0)
#define __builtin_arm_ldsmin(...) ((unsigned long long)0)
#define __builtin_arm_ldumax(...) ((unsigned long long)0)
#define __builtin_arm_ldumin(...) ((unsigned long long)0)
#endif

#if !__has_builtin(__builtin_arm_pacga)
#define __builtin_arm_pacga(...) ((unsigned long long)0)
#endif
#if !__has_builtin(__builtin_arm_addg)
#define __builtin_arm_addg(...) ((void *)0)
#endif
#if !__has_builtin(__builtin_arm_subg)
#define __builtin_arm_subg(...) ((void *)0)
#endif
#if !__has_builtin(__builtin_arm_wsp_write)
#define __builtin_arm_wsp_write(...) ((void)0)
#endif
#if !__has_builtin(__builtin_arm_wsp_zero_extend)
#define __builtin_arm_wsp_zero_extend(...) ((unsigned long long)0)
#endif

#endif
