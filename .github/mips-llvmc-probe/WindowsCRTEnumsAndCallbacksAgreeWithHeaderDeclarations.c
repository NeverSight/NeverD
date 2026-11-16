#include <stdint.h>
enum _crt_app_type { _crt_unknown_app, _crt_console_app, _crt_gui_app };
enum _crt_argv_mode { argv_none, argv_plain, argv_expanded };
struct _exception { int type; };
typedef void (*_PVFV)(void);
typedef int (*_onexit_t)(void);
typedef struct _onexit_table_t {
  _PVFV *_first, *_last, *_end;
} _onexit_table_t;
typedef void (__attribute__((stdcall)) *_tls_callback_type)(
    void *, unsigned long, void *);
void _set_app_type(enum _crt_app_type);
int _configure_narrow_argv(enum _crt_argv_mode);
int _configure_wide_argv(enum _crt_argv_mode);
void __setusermatherr(int (*)(struct _exception *));
int _initialize_onexit_table(_onexit_table_t *);
int _register_onexit_function(_onexit_table_t *, _onexit_t);
int _execute_onexit_table(_onexit_table_t *);
void _register_thread_local_exe_atexit_callback(_tls_callback_type);
#if !defined(__SIZEOF_INT128__) && defined(__BITINT_MAXWIDTH__) && __BITINT_MAXWIDTH__ >= 128
typedef unsigned _BitInt(128) __uint128_t;
typedef _BitInt(128) __int128_t;
#endif


extern void _set_app_type(enum _crt_app_type);
extern int32_t _configure_narrow_argv(enum _crt_argv_mode);
extern int32_t _configure_wide_argv(enum _crt_argv_mode);
extern void __setusermatherr(int32_t (*)(struct _exception *));
extern int32_t _initialize_onexit_table(struct _onexit_table_t *);
extern int32_t _register_onexit_function(struct _onexit_table_t *, int32_t (*)(void));
extern int32_t _execute_onexit_table(struct _onexit_table_t *);
extern void _register_thread_local_exe_atexit_callback(_tls_callback_type);
uint32_t configure_crt(uint32_t mode, void* callback, void* table, void* onexit, void* tls);

uint32_t configure_crt(uint32_t mode, void* callback, void* table, void* onexit, void* tls) {
    uint32_t narrow0;
    uint32_t wide1;
    uint32_t init2;
    uint32_t reg3;
    uint32_t exec4;

    _set_app_type(mode);
    narrow0 = _configure_narrow_argv(mode);
    wide1 = _configure_wide_argv(mode);
    __setusermatherr((int32_t (*)(struct _exception *))(uintptr_t)(callback));
    init2 = _initialize_onexit_table((struct _onexit_table_t *)(uintptr_t)(table));
    reg3 = _register_onexit_function((struct _onexit_table_t *)(uintptr_t)(table), (int32_t (*)(void))(uintptr_t)(onexit));
    exec4 = _execute_onexit_table((struct _onexit_table_t *)(uintptr_t)(table));
    _register_thread_local_exe_atexit_callback((_tls_callback_type)(uintptr_t)(tls));
    return ((narrow0 + wide1) + ((init2 + reg3) + exec4));
}


static int app, narrow, wide, observed, exit_observed, tls_observed;
static _onexit_table_t *initialized;
static _PVFV exit_slot;
static _onexit_t exit_callback;
void _set_app_type(enum _crt_app_type value) { app = value; }
int _configure_narrow_argv(enum _crt_argv_mode value) {
  narrow = value;
  return value + 1;
}
int _configure_wide_argv(enum _crt_argv_mode value) {
  wide = value;
  return value + 2;
}
void __setusermatherr(int (*callback)(struct _exception *)) {
  struct _exception error = {41};
  observed = callback(&error);
}
static int handle_math_error(struct _exception *error) {
  return error->type + 1;
}
int _initialize_onexit_table(_onexit_table_t *table) {
  initialized = table;
  table->_first = &exit_slot;
  table->_last = table->_first;
  table->_end = table->_first + 1;
  return 3;
}
int _register_onexit_function(_onexit_table_t *table, _onexit_t callback) {
  if (table != initialized || table->_first != &exit_slot ||
      table->_last != table->_first || table->_end != table->_first + 1)
    __builtin_trap();
  exit_callback = callback;
  return 5;
}
int _execute_onexit_table(_onexit_table_t *table) {
  if (table != initialized) __builtin_trap();
  return exit_callback();
}
static int handle_exit(void) {
  ++exit_observed;
  return 11;
}
void _register_thread_local_exe_atexit_callback(_tls_callback_type callback) {
  callback(initialized, 0x12345678UL, &exit_slot);
}
static void __attribute__((stdcall)) handle_tls(
    void *module, unsigned long reason, void *reserved) {
  tls_observed = module == initialized && reason == 0x12345678UL &&
                 reserved == &exit_slot;
}
int main(void) {
  _onexit_table_t table;
  return configure_crt(2, (void *)handle_math_error, &table,
                       (void *)handle_exit, (void *)handle_tls) != 26 ||
         app != 2 || narrow != 2 || wide != 2 || observed != 42 ||
         exit_observed != 1 || tls_observed != 1;
}
