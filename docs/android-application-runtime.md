# Android application and class loader snapshots

The API 28 native profile can project a declared process lifecycle into the
shared Java heap. JNI and DEX use the same objects and methods. This is an
explicit analysis input, not discovery of the installed application or execution
of the Android framework's startup code.

```json
{
  "android": {
    "jni": true,
    "application_snapshot": {
      "phase": "application",
      "package_name": "example.application"
    }
  }
}
```

The snapshot requires JNI. Its phase is mandatory in JSON:

| Phase | `ActivityThread.currentActivityThread()` | `currentApplication()` / `getApplication()` |
| --- | --- | --- |
| `unattached` | null | null (instance calls still require a receiver) |
| `attached` | retained ActivityThread | null |
| `application` | retained ActivityThread | retained platform Application |

An absent snapshot is unknown and stops at the first framework operation. It
does not select `unattached`. `package_name` is optional, at most 1024 UTF-8 bytes,
nonempty, and allowed only in the application phase. An absent name stops
`getPackageName`; properties and DEX names are not used to guess it.

The application phase represents a base `android.app.Application` already
attached to a distinct `ContextImpl`. `getBaseContext()` returns that context;
both objects' `getApplicationContext()` return the same Application and their
`getClassLoader()` return the same application PathClassLoader. Local-reference
deletion, local frames, native thread attachment and detachment do not reset
these workload-owned identities. Two native threads see the same objects.

Three loader identities are separate: the boot loader, the system PathClassLoader
and the application PathClassLoader. System and application loaders have the
boot loader as parent; the boot parent is null. On API 28 a platform class's
`Class.getClassLoader()` returns the BootClassLoader object. Consequently the
base Application object's class loader differs from its Context's application
loader. Supplied DEX classes use the application loader; array classes use
their component's loader, including the boot loader for primitive arrays.

`ClassLoader.loadClass(String)` accepts known ASCII binary names. The application
loader can resolve supplied DEX declarations and the admitted platform classes;
system and boot loaders resolve only the admitted platform classes. Loading
does not execute a supplied class initializer, including after that initializer
has failed. Null names throw NullPointerException. Unprovided names, array or
descriptor spellings, non-ASCII lookup and unknown loader instances stop
explicitly. They do not become ClassNotFoundException, empty classes, host
classpath searches or successful placeholder objects.

The lifecycle phase is fixed per workload; modeled mutable fields retain guest writes. It does not execute
`bindApplication`, custom Application constructors, framework class initializers,
split/delegate-last/custom class loaders, security-manager policies, framework
callbacks, a Java scheduler, `Class.forName` or native Java method bodies.
RegisterNatives metadata remains separate from Java-to-native execution. The
model does not give unmodeled framework fields or methods inferred values.
Native evidence obtained under a snapshot remains conditional on that input.

`dalvik::Runtime` owns the projection and its retained identities; Library owns
common argument, receiver, type and exception checks. DEX and JNI share this
boundary and the workload budget. A non-Java stop poisons the Runtime, including
partial allocation; it cannot be retried as a fresh snapshot.

Behavior references are Android's
[ActivityThread](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/app/ActivityThread.java),
[ContextImpl](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/app/ContextImpl.java),
[ContextWrapper](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/content/ContextWrapper.java),
[ApplicationLoaders](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/app/ApplicationLoaders.java),
[Class](https://android.googlesource.com/platform/libcore/+/android-9.0.0_r1/ojluni/src/main/java/java/lang/Class.java)
and [ClassLoader](https://android.googlesource.com/platform/libcore/+/android-9.0.0_r1/ojluni/src/main/java/java/lang/ClassLoader.java)
at `android-9.0.0_r1`. The implementation and synthetic tests are independently
written; reference framework bodies are not embedded in NeverD.

## Instrumentation field state

`application_snapshot.instrumentation` explicitly selects `uninitialized`
(a null field) or `platform` (one retained `android.app.Instrumentation` object).
It is optional for `attached` and `application`, and invalid for `unattached`.
Application creation does not select an instrumentation value: API 28 binding
installs instrumentation before creating the Application and can use a custom
subclass. An omitted value therefore stops when read, rather than implying null
or the default implementation.

The declared package-private `ActivityThread.mInstrumentation` instance field and
public `getInstrumentation()` method share one Runtime-owned value. JNI resolves
its actual field ID and reads/writes through the same owner used by DEX method
execution. A guest write, including null, establishes the value without reading
an unknown initial value; compatible supplied subclass objects retain identity.
Deleted JNI local references and native detach/reattach do not reset the field.
An unrelated ActivityThread object cannot borrow the process snapshot's state.
Unknown fields, custom instrumentation startup and callbacks remain unsupported;
this projection does not execute framework constructors, `basicInit` or services.

## Existing application files directory

`application_snapshot.files_directory` is optional in the `application` phase.
It names an already initialized, existing private files directory; it does not
infer a package, create a directory, change permissions or run ContextImpl's
initialization code. The native profile requires `linux_files` and an exact
entry in `linux_files.directories` (root is intrinsic). Missing or conflicting
namespace inputs fail before execution. Standalone Dalvik validates an absolute,
NUL-free UTF-8 path of at most 4095 bytes and applies File's Unix lexical slash
normalization; it does not resolve dot components or consult the host.

`Context.getFilesDir()` on the retained application or base context returns the
same workload-owned `java.io.File`. Its `getPath`, `getAbsolutePath` and `toString`
return the retained path string; `getName`, `getParent` and `isAbsolute` provide
lexical queries over that absolute path. Root has an empty name and null parent.
Unknown paths and foreign File objects stop explicitly. Arbitrary File
constructors, relative Java paths, canonicalization, filesystem queries and
mutations are outside this existing-directory projection. Directory availability
is not evidence of the actual installed application.

## Guest working directory

`linux_files.working_directory` optionally supplies a linked, reachable guest
cwd, independently of the application's files directory. It must be `/` or a
canonical absolute entry in `linux_files.directories`. An implicit catalogue
parent alone is insufficient for this explicit input. No host cwd is inherited.

Raw x64/AArch64 `getcwd` returns the bytes copied including NUL. A short buffer
returns `ERANGE` before output access, wholly inaccessible output returns
`EFAULT`, and mixed writable/unwritable output stops without inventing an
architecture-specific partial copy. Android Bionic `getcwd` returns the original
buffer on success and null with thread-local errno on failure. A non-null buffer
with size zero returns `EINVAL`; a null buffer allocates guest memory, and size
zero follows API 28's page allocation, exact string duplication and temporary
release. Guest `free` owns returned allocations. Successful calls preserve errno.
An absent cwd remains unknown. These inputs do not implement `chdir`, deleted
or unreachable cwd, relative pathname traversal, mounts or filesystem mutation.

Behavior references are API 28 [File](https://android.googlesource.com/platform/libcore/+/android-9.0.0_r1/ojluni/src/main/java/java/io/File.java),
[UnixFileSystem](https://android.googlesource.com/platform/libcore/+/android-9.0.0_r1/ojluni/src/main/java/java/io/UnixFileSystem.java)
and [Bionic getcwd](https://android.googlesource.com/platform/bionic/+/android-9.0.0_r1/libc/bionic/getcwd.cpp).
Implementations and synthetic callers are independently written.
