/* Authored API 28 callers. All environment storage belongs to the guest. */
typedef unsigned long U;
extern char *getenv(const char *);
extern int *__errno(void);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);
extern int pthread_create(U *, const void *, void *(*)(void *), void *);
extern int pthread_join(U, void **);

static U InitialValue, InitialErrno;
__attribute__((constructor)) static void initialize(void) {
  *__errno() = 91;
  InitialValue = (U)getenv("COLOR");
  InitialErrno = (unsigned)*__errno();
}

U environment_query(U *out, const char *name, U mode) {
  char *(*query)(const char *) = getenv;
  if (mode) {
    void *library = dlopen("libenvironment.so", 2);
    query = (char *(*)(const char *))dlsym(library, "getenv");
    if (!query)
      return 1;
    if (mode == 2)
      dlclose(library);
  }
  *__errno() = 77;
  out[0] = (U)query(name);
  out[1] = (unsigned)*__errno();
  out[2] = InitialValue;
  out[3] = InitialErrno;
  return 0;
}

U environment_mutate(U *out, char **vector) {
  char *first = getenv("COLOR");
  if (!first)
    return 1;
  out[0] = (U)first;
  first[0] = 'R';
  out[1] = (U)getenv("COLOR");
  out[2] = (unsigned char)*getenv("COLOR");
  vector[0] = vector[1];
  out[3] = (U)getenv("COLOR");
  vector[0] = 0;
  out[4] = (U)getenv("COLOR");
  return 0;
}

static void *worker(void *argument) {
  U *out = argument;
  *__errno() = 88;
  char *value = getenv("COLOR");
  out[0] = (U)value;
  out[1] = (unsigned)*__errno();
  if (value)
    value[0] = 'T';
  return 0;
}

U environment_thread(U *out) {
  U thread;
  *__errno() = 77;
  if (pthread_create(&thread, 0, worker, out))
    return 1;
  if (pthread_join(thread, 0))
    return 2;
  char *value = getenv("COLOR");
  out[2] = (U)value;
  out[3] = (unsigned)*__errno();
  out[4] = value ? (unsigned char)*value : 0;
  return 0;
}

/* Indirect references remain functions when the optimizer removes the PLT. */
__asm__(".type getenv, @function");
