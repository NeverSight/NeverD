/* Keep this launcher native so the app bundle has no signed script in MacOS/.
 */
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  uint32_t capacity = 0;
  (void)_NSGetExecutablePath(NULL, &capacity);
  char *executable = malloc(capacity);
  if (!executable || _NSGetExecutablePath(executable, &capacity) != 0) {
    fputs("neverd-mcp: cannot locate application bundle\n", stderr);
    return 1;
  }
  char *directory = realpath(executable, NULL);
  free(executable);
  char *separator = directory ? strrchr(directory, '/') : NULL;
  if (!separator) {
    fputs("neverd-mcp: invalid application location\n", stderr);
    free(directory);
    return 1;
  }
  *separator = '\0';
  char *script = NULL;
  char *interpreter = NULL;
  if (asprintf(&script, "%s/../Resources/mcp/server.py", directory) < 0)
    script = NULL;
#ifdef NEVERD_BUNDLED_PYTHON
  if (asprintf(&interpreter,
               "%s/../Frameworks/Python.framework/Versions/%s/bin/python%s",
               directory, NEVERD_BUNDLED_PYTHON, NEVERD_BUNDLED_PYTHON) < 0)
    interpreter = NULL;
#else
  interpreter = strdup("python3");
#endif
  free(directory);
  char **arguments = calloc((size_t)argc + 3, sizeof(char *));
  if (!script || !interpreter || !arguments) {
    fputs("neverd-mcp: cannot allocate launcher arguments\n", stderr);
    free(script);
    free(interpreter);
    free(arguments);
    return 1;
  }
  arguments[0] = interpreter;
  /* Keep Python bytecode caches out of the signed application resources. */
  arguments[1] = "-B";
  arguments[2] = script;
  for (int index = 1; index < argc; ++index)
    arguments[index + 2] = argv[index];
  execvp(interpreter, arguments);
  perror("neverd-mcp: Python 3.10+ is required");
  free(script);
  free(interpreter);
  free(arguments);
  return 1;
}
