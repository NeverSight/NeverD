# Android application and class loader snapshots

`application_info.split_source_dirs` and `split_public_source_dirs` each
accept an omitted field (unknown),
`null` (known null array), or an ordered array of nullable Strings, including
an empty array. The same shape applies to application queries and nested
PackageInfo responses. The bounded projection admits at most 64 elements of at
most 4096 UTF-8 bytes each. These are model input limits, not Android package
limits. Array objects and elements are retained and mutable; separate parcel
responses and the two fields have independent arrays. No public/private
path fallback or implicit alias is inferred. Values do not assert file existence,
canonicalize paths or execute split loading.

The API 28 native profile can project a declared process lifecycle into the
shared Java heap. JNI and DEX use the same objects and methods. This is an
explicit analysis input, not discovery of the installed application or execution
of the Android framework's startup code.

`main_looper_prepared` supplies stable API 28 `Looper.sMainLooper` state,
independent of the Application phase. Omission is unknown, `false` returns
null, and `true` retains one Looper and its MessageQueue identity.
`Handler(Looper)` copies that exact queue and Looper into ordinary final
instance fields, with null callback and synchronous delivery mode.
A null constructor argument throws
`NullPointerException`. JNI and DEX share these fields and identities.
JNI `AllocObject` instead retains ordinary zero fields: its Looper has a known
null queue, and its unconstructed Handler has a null Looper. An object without
modeled queue storage remains unsupported. This projection does not run
`prepareMainLooper` or assign a Java thread. Posting requires the additional
state below; callback dispatch still requires separate execution.

`Handler.post(Runnable)` admits an explicitly empty initial Message pool
(`initial_message_pool_empty: true`) and main queue
(`initial_main_queue: {"blocked": false}`, requiring
`main_looper_prepared: true`). Missing state is unknown; a declared nonempty
pool remains unsupported. These declarations are initial conditions, not fixed
responses: each post allocates a distinct Message, retains its exact Handler and
nullable Runnable, and changes the shared heap-linked queue. Message defaults
include sendingUid -1. Equal nonzero deadlines preserve insertion order; zero
goes to the front. At most 1024 pending messages are admitted.

The native profile reads uptime through its existing Linux monotonic clock,
including instruction and idle advancement. No second timestamp input or host
clock is used. Standalone Runtime callers provide the same capability through
`RuntimeServices::uptime_milliseconds`; omission stops at the clock boundary.
Negative or overflowing native nanosecond observations remain unsupported.

The final post declaration cannot be overridden by supplied DEX. Its virtual
sendMessageAtTime boundary is preserved: supplied overrides stop explicitly
until their invocation is modeled. A null queue stops at its logging boundary.
An initially blocked queue enqueues, then stops at unmodeled nativeWake; the
partial queue remains observable and is not reported as a successful post.
The read-only `java_state.framework.main_message_queue` report traverses actual
Message fields and marks malformed or cyclic lists incomplete. It contains
model object identities, not JNI handles. Queue retention survives native thread
detach. No accepted post executes the Runnable, creates a Java thread, removes
messages, or implies startup completion. Message recycling, arbitrary initial
queues, Looper dispatch, and Java monitor scheduling remain outside this scope.

The independently written model follows API 28
[Handler](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/os/Handler.java),
[Message](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/os/Message.java),
[MessageQueue](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/os/MessageQueue.java),
and [SystemClock](https://android.googlesource.com/platform/system/core/+/android-9.0.0_r1/libutils/SystemClock.cpp).
Framework source bodies are not embedded in NeverD.

`Looper.getQueue()`, `Message.getTarget()` and `Message.getCallback()` read
the same retained typed fields as JNI and DEX. Their results are actual
references, including known null fields of ordinary allocated objects.
`MessageQueue.next()` is package-private in API 28. The modeled JNI method
lookup can call it; ordinary application DEX invocation is explicitly
unsupported rather than admitting it as a public framework method.

Taking a message requires `initial_main_queue.native_callbacks_empty: true`
in addition to the known initial queue. This input declares that the native
poll with timeout zero has no callbacks, timers or descriptor listeners.
Omission remains unknown and false remains unsupported. The model reads the
shared uptime clock, validates the bounded retained chain, and removes only
an immediately due message. A head synchronization barrier selects the first
following asynchronous message; a future first candidate does not permit
skipping to a later ready message. Removal clears its next link, marks it in
use and clears the queue's blocked state without executing its callback.
Unknown native state, malformed links, unknown fields and work exhaustion
leave the queue links unchanged. Inspection, posting and removal use the
single message-count policy in `neverd/dalvik/Limits.h`.

Empty or delayed queues still stop at the unmodeled blocking/idle boundary;
they do not return a fabricated null. Quit/dispose, recycling, idle handlers,
native polling with callbacks and full `Looper.loop()` dispatch remain
unmodeled. Returning a live message does not establish Android startup or
callback execution. These semantics follow the API 28 sources cited above
and [Looper](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/os/Looper.java).

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

`application_loader_established` optionally supplies the independent loader
stage. With `phase: attached`, `true` makes the application PathClassLoader
available through supplied classes' `Class.getClassLoader` while
`currentApplication`/`getApplication` still return null. The loader retains the
boot parent and exact supplied-declaration namespace across thread detach.
Explicit application-loader lookup failures are also admitted in this stage.
Absent means unknown in `attached`; `false` explicitly declares it unestablished.
Neither case permits application-class loader queries to return a guessed value.
`unattached` implies false and `application` implies true; contradictory explicit
values are rejected. This separates loader setup from Application construction,
as in API 28 [LoadedApk.makeApplication](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/app/LoadedApk.java).
It does not infer the path, APK identity, custom factory or constructor body.

The application phase represents a base `android.app.Application` already
attached to a distinct `ContextImpl`. `getBaseContext()` returns that context;
both objects' `getApplicationContext()` return the same Application and their
`getClassLoader()` return the same application PathClassLoader. Local-reference
deletion, local frames, native thread attachment and detachment do not reset
these workload-owned identities. Two native threads see the same objects.

`package_resource_path` optionally supplies the attached Application's stable
API 28 `LoadedApk.mResDir`, returned by `Context.getPackageResourcePath()`.
Missing input is unknown, JSON null is a known null Java value, and a String
is preserved exactly up to 4096 UTF-8/WTF-8 bytes. The Application and its base
Context share the retained result across DEX/JNI calls and native thread detach.
No path normalization, file access or resource loading is inferred. Writes to
the public ApplicationInfo fields do not change this separate retained value.
The snapshot requires the application phase; system and foreign contexts are
unsupported. It does not infer sourceDir/publicSourceDir selection or changes
to LoadedApk after the snapshot. This follows API 28
[ContextImpl.getPackageResourcePath](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/app/ContextImpl.java)
and [LoadedApk.getResDir](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/app/LoadedApk.java).

`package_manager_available` optionally declares stable package-service
availability for this Context. Absence is unknown and stops
`Context.getPackageManager()`; false returns null. True permits one retained
`android.app.ApplicationPackageManager`, shared by the Application and its
base Context across JNI/DEX calls and native thread detach/reattach. This
concrete class extends the abstract `android.content.pm.PackageManager`.
Availability does not create an Application or authorize foreign Context
instances. Service acquisition and Binder transactions remain unsupported;
supplying APK bytes does not establish this input.
The contract follows API 28
[ContextImpl.getPackageManager](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/app/ContextImpl.java)
and [ApplicationPackageManager](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/app/ApplicationPackageManager.java).

`application_info_queries` supplies up to 64 distinct positive responses to
`PackageManager.getApplicationInfo(String,int)` for that Context's own user.
Each entry has exactly `package_name`, signed 32-bit `flags`, and `info`.
Flags accept exact JSON integers or decimal strings through the shared integer
reader; booleans, fractional values and out-of-range values are rejected.
Names contain 1–1024 UTF-8 bytes without NUL. `info` uses the same required
nullable `source_dir` and optional nullable `native_library_dir` and
`non_localized_label` schema as
`application_info`; each String is limited to 4096 UTF-8 bytes. The shared
validator requires an Application and known available package service for any
nonempty inventory. Responses declare the final API 28 result after ABI path
adjustment. They do not run a package service or infer responses from APK files.
The method matches the exact UTF-16 name and Java int flags. An unknown key,
null name or foreign manager stops explicitly; a missing record is not a
negative lookup or `NameNotFoundException`.

Each positive query creates an independent ApplicationInfo parcel result.
Mutable field writes remain attached to that returned object across DEX/JNI
calls and native thread detach; they do not affect later query results or the
Context's retained ApplicationInfo. Unknown optional fields stay unknown until
an explicit guest write supplies them. Query matching, object allocation and
field materialization use the existing work and storage budgets.

`package_info_queries` independently supplies up to 64 positive own-user
`PackageManager.getPackageInfo(String, int)` results. It uses the same exact
`package_name`, int32 `flags` and `info` record keys, query matching, lifecycle
requirements and budgets as ApplicationInfo queries. Its `info` object admits
`package_name`, `version_name`, `version_code` and `application_info`, all optional.
String fields distinguish unknown, known null and exact UTF-8/WTF-8 text up to 4096
bytes; version_code is an explicit signed int32. Empty info leaves all four
fields unknown. A query key does not infer its returned packageName field.

Each result is a fresh Parcelable PackageInfo with independently mutable
`packageName`, `versionName`, `versionCode` and `applicationInfo` fields. JNI and
DEX share Runtime reference validation and primitive narrowing before the field owner stores a
write. Guest writes can establish unknown fields and survive native detach.
The `application_info` member is null or an object using the ApplicationInfo
schema below, including its required `source_dir`. Omission leaves the field
unknown. A supplied object is retained within that PackageInfo result; separate
queries create independent nested objects and mutable fields. It is not the
Context's retained ApplicationInfo or an ApplicationInfo query result. Its
fields and inherited label behavior use the same shared owner, and replacing
the parent reference does not rewrite the previous child. Field declaration
lookup itself does not require a supplied response or field value.
There is no APK parsing or package-service execution in this projection.
Missing query records, foreign instances, all other PackageInfo members and
negative lookups remain explicit unsupported cases. This contract follows API 28
[PackageInfo](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/content/pm/PackageInfo.java)
and [ApplicationPackageManager.getPackageInfo](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/app/ApplicationPackageManager.java).

An optional `application_info` object with a required `source_dir` member
supplies the initial Java `ApplicationInfo.sourceDir` field. It requires the
application phase. The member is null or a UTF-8/WTF-8 string of at most 4096
bytes; missing outer input means unknown. The String is preserved exactly,
including empty values, without path normalization or filesystem inference.
An optional `native_library_dir` member independently supplies `nativeLibraryDir`
under the same nullable String and byte-limit rules. Its absence is unknown even
when sourceDir is known; no library directory is derived from the APK path.
`non_localized_label` optionally supplies the inherited
`PackageItemInfo.nonLocalizedLabel` field, initially as a String or known null.
It has the same 4096-byte validation bound and independent unknown state.
Its public `CharSequence` field also retains compatible guest writes, including
mutable StringBuilder identities; JNI and DEX use the same field owner.
Optional `target_sdk_version` supplies the
independent mutable `targetSdkVersion` int32 field, including an explicitly
known zero. Missing input stays unknown; neither the platform SDK property nor
APK bytes supply it implicitly. Context and package-query results retain
separate values, and guest writes use Runtime's common 32-bit field conversion.
`Context.getApplicationInfo()` retains one ApplicationInfo shared by the
Application and its base Context. JNI and DEX reads/writes access the same
mutable fields and String identities. A guest write may establish an initially
unknown value. Foreign instances and all other ApplicationInfo fields,
constructors and parcel operations remain unsupported. No APK, package-manager
transaction or framework startup is executed by this projection.
The contract follows API 28 [ApplicationInfo](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/content/pm/ApplicationInfo.java),
[ContextImpl](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/app/ContextImpl.java)
and [LoadedApk](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/app/LoadedApk.java).
The implementation and tests are independently authored.

Optional `default_locale` supplies a stable process default Locale independently
of application attachment. The object may contain `language` and `country`,
each an already-normalized API 28 BaseLocale UTF-8/WTF-8 string of at most 1024
bytes. Missing object or components remain unknown; empty strings are known
values and null is invalid. Language rejects ASCII uppercase and the modern
codes `he`, `yi`, `id` (API 28 stores `iw`, `ji`, `in`); country rejects ASCII
lowercase. This validates a snapshot rather than running a Locale constructor.
`Locale.getDefault()` retains one nonnull object across JNI threads and DEX
calls. Its `getLanguage()` and `getCountry()` return the shared heap's interned
component Strings. No locale is inferred from the host, properties, APK or
application phase. Foreign instances, constructors, setters, category-specific
defaults, other Locale methods and initialization remain unsupported.
The contract follows API 28
[Locale](https://android.googlesource.com/platform/libcore/+/android-9.0.0_r1/ojluni/src/main/java/java/util/Locale.java)
and [BaseLocale](https://android.googlesource.com/platform/libcore/+/android-9.0.0_r1/ojluni/src/main/java/sun/util/locale/BaseLocale.java);
the implementation and tests are independently authored.

Optional `application_loader_paths` and `system_loader_paths` independently
supply the retained PathClassLoader's stable DexPathList state. Each object
requires all three String arrays: `dex_zip_files`,
`native_library_directories`, and `system_native_library_directories`.
Empty inventories are known; absent outer objects remain unknown. The application
input requires an established application loader, including the explicit
attached-phase condition. Each array is limited to 64 entries, each File path to
4096 UTF-8/WTF-8 bytes, and the combined three arrays to 16384 bytes. Path spelling
and ordering are preserved.
`BaseDexClassLoader.toString()` and `PathClassLoader.toString()` use this shared
owner and return a fresh String. This subset admits ZIP elements with a nonnull
DexFile and nonnull File path, so it does not require a directory probe when
formatting an element. Native directories precede system directories. File paths
are explicit state; opening DEX files, library searches, loader construction,
other element forms and path-list mutation are outside this projection.
The format follows API 28
[BaseDexClassLoader](https://android.googlesource.com/platform/libcore/+/android-9.0.0_r1/dalvik/src/main/java/dalvik/system/BaseDexClassLoader.java)
and [DexPathList](https://android.googlesource.com/platform/libcore/+/android-9.0.0_r1/dalvik/src/main/java/dalvik/system/DexPathList.java).

`package_item_force_safe_labels` declares the stable API 28 PackageItemInfo
static mode. Its absence is unknown. `PackageManager.getApplicationLabel(info)`
requires the workload's own manager and a retained Context or query result.
A null info argument throws NullPointerException. With the mode explicitly
false and nonLocalizedLabel nonnull, it returns that exact CharSequence,
including whitespace and mutable object identity. No trim, String conversion
or resource access occurs on this branch. Unknown mode/field state, true mode,
null-label resource/name fallback and foreign objects remain explicit stops.
The mode is not inferred from the Java static initializer or APK bytes.
This contract follows API 28
[PackageItemInfo.loadLabel/loadUnsafeLabel](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/content/pm/PackageItemInfo.java)
and [ApplicationPackageManager.getApplicationLabel](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/app/ApplicationPackageManager.java).

An optional `secure_settings_cache` object with exactly one `android_id` member
declares an already populated, generation-stable API 28 settings cache for the
attached resolver's own process user. Its value is a string or JSON null; an
absent cache is unknown. The same typed snapshot validates at most 4096 UTF-8
bytes and requires the Application phase. This explicit cache precondition is
consumed by `Settings.Secure.getString(resolver, "android_id")`: known absence
returns null, while a string keeps one retained identity across DEX/JNI calls.
Empty and null are distinct. A null resolver throws NullPointerException.
Foreign resolvers, undeclared keys, cache invalidation, user switching, writes and
provider acquisition remain unsupported. No identifier is generated by the
runtime and no Binder or settings-provider exchange is inferred.

An independent `global_settings_cache` object with exactly one `adb_enabled`
member declares the equivalent Global cache state. API 28 routes this deprecated
Secure key to Global; it cannot read `secure_settings_cache.android_id`.
The same nullable string, lifecycle, encoding and size rules apply.
`Settings.Secure.getInt(resolver, key, default)` reads the same cached string and
supports ASCII decimal int32 conversion: optional sign, leading zeros and both
signed bounds. Known null, empty or invalid ASCII text and overflow return the
caller's int32 default. Unknown state still stops, regardless of that default.
Non-ASCII integer conversion remains unsupported because it requires the
platform's versioned `Character.digit` tables. String reads retain those values.
The two-argument throwing overload and all unlisted keys remain unsupported.

`Settings.Secure.ADB_ENABLED` and `ANDROID_ID` are API 28 public static final
String keys, `"adb_enabled"` and `"android_id"`. They use Runtime's existing
static-field initialization and shared literal pool, without application or
provider input. Reading a key does not supply the setting's value. Writes to
these final fields and undeclared fields retain the ordinary refusal paths.

`Context.getContentResolver()` returns the retained resolver of that attached
ContextImpl. Calls through the Application and its base context share the same
`android.app.ContextImpl$ApplicationContentResolver` instance, a subclass of
the abstract `android.content.ContentResolver`. Resolving it requires no
package-name or files-directory input. It does not acquire a content provider,
perform a Binder transaction, supply settings values, or execute framework
constructors. Those operations remain unsupported. Unknown lifecycle state
and foreign Context instances cannot borrow this resolver. JNI reference
deletion, detach/reattach and DEX invocation preserve its workload identity.
Allocation and work limits remain terminal, as for the other framework objects.

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

An optional `application_snapshot.class_lookup_failures` array declares exact
negative `loadClass` observations. Each entry requires `loader` (`boot`,
`system` or `application`), exactly one of `binary_name` or `exact_query`,
and `message` (a string or explicit null). For example:

```json
{"phase":"attached","class_lookup_failures":[
  {"loader":"boot","binary_name":"example.Optional","message":null},
  {"loader":"boot","exact_query":"example.Other ","message":null}
]}
```

This input is conditional evidence, not automatic inference from an incomplete
DEX inventory. Only the selected loader's exact lookup throws a fresh shared
`ClassNotFoundException` with the declared message. The guest can catch or clear
it and continue; known loads still avoid initialization. Failure in a parent
does not declare failure in a child, nor does it alter JNI `FindClass` outcomes.
Unknown names and foreign loader instances still stop explicitly. The array is
limited to 256 unique loader/query pairs and messages to 4096 UTF-8 bytes.
`binary_name` accepts up to 1024 ASCII bytes with nonempty Java identifier
components. `exact_query` instead accepts a nonempty string of at most 1024
printable ASCII bytes, including spaces. It preserves every byte without
trimming or normalization; a query with a trailing space is distinct from the
name without that space. Slash spellings, array prefixes, controls and non-ASCII
queries remain unsupported. Duplicate loader/query pairs are rejected across
both input forms. In C++, supplying `exact_query` requires an empty
`binary_name`. Empty messages and null messages remain distinct. Application
loader outcomes require
an established application loader: the application phase or an explicit `true`
loader input in the attached phase. A failure cannot shadow a declaration visible
to that loader. Custom delegation, exception causes and a closed complete
classpath are not supplied by these observations.

The lifecycle phase is fixed per workload; modeled mutable fields retain guest writes. It does not execute
`bindApplication`, custom Application constructors, framework class initializers,
split/delegate-last/custom class loaders, security-manager policies, framework
callbacks, a Java scheduler or `Class.forName`. DEX invoke instructions and JNI calls to registered native declarations
use the bounded guest-call contract below; registration alone executes nothing. The
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
[ContentResolver](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/content/ContentResolver.java),
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

## Read-only Java state directory

Set `android.observe_java_state: true` with `jni: true` to include
`android.java_state` at the final native workload stop. This reads the shared
Runtime, including after a Java/resource failure. It does not initialize a
class, look up a new Java object, consume guest instructions, or create loaders.
The default is off. The C++ equivalent is `Runtime::snapshot()`.

`classes` lists the admitted Program declarations in descriptor order, with
their superclass, `source_id`, `supplied` ownership, class-object identity and
initialization state (`not_started`, `prepared`, `initializing`, `initialized`,
or `erroneous`). `initialized` records Runtime state; it does not imply that
a `<clinit>` body existed. Native DEX source IDs are `supplied-dex-N`, indexing the explicit
input images. Source labels do not establish ownership: `supplied` is recorded
by Program independently. Library-only intrinsics and array types are outside
this declaration inventory; it is not a complete Android boot classpath.

`framework` separates `declared_phase` from the identities materialized by
actual model operations. Zero means not materialized, not proof of absence on a
device. `instrumentation: null` means unknown/unread, whereas `"0"` is a known
null value. Identities are workload heap tokens, **not JNI handles or guest
addresses**. `terminal` means Runtime has retained a non-Java stop; an ordinary
Java exception instead retains the class's erroneous initialization state.

`projected_loader` uses the same ownership decision as `Class.getClassLoader`.
It is null when no lifecycle snapshot establishes that ownership. It records
boot/application ownership in the current single-application-loader projection,
not the actual definer of an unavailable APK. The system loader is distinct
from the application loader; both have the boot parent in this projection.
Reading this directory does not turn a declared Application snapshot into
execution of `LoadedApk.makeApplication`, a custom constructor or `onCreate`.

## Calling registered native methods through JNI

`GetMethodID` and `GetStaticMethodID` resolve supplied declarations independently
of body support and current native registration. Their active class
initialization still runs through Runtime; a missing library-load operation or
throwing initializer cannot be skipped by registration.

The JNI `Call<Type>Method`, `CallNonvirtual<Type>Method` and
`CallStatic<Type>Method` families, including their `A` and `V` forms, can invoke
registered ordinary native methods. Virtual dispatch selects the supplied
receiver override; nonvirtual dispatch keeps the selected declaring method.
The registration is read at invocation, so rebinding affects existing method
IDs and unregistering removes their target. Unknown native symbol lookup stops
explicitly rather than guessing a library or throwing an inferred link error.

Android's existing guest callback scheduler executes the registered AArch64
address on the calling thread, with its JNIEnv and a fresh local reference to
the receiver or declaring class. Up to six integral/reference Java arguments
fit the admitted register ABI. The shared Java heap validates object types and
retains mutations. Native callbacks can make nested JNI calls and wait in
modeled Bionic services; other threads retain their own frames and environments.
The existing workload instruction/deadline limits include original native work.

Each native invocation has an implicit local frame. A returned object is
validated before the callee's local frames retire, then gets a caller-local
reference to the same heap object. Globals and caller locals survive. Pending
Java exceptions survive retirement and supersede an unspecified native return
carrier. `PopLocalFrame` cannot pop the implicit frame, and detach during a
native Java invocation returns JNI_ERR without destroying the attachment.
Terminal guest faults or unsupported operations remain terminal; they are not
reported as a successful Java return.

Floating arguments/results, stack-passed native arguments, synchronized native
methods, dynamic JNI symbol resolution and DEX-to-native continuations remain
unsupported. The DEX reader rejects unsupported ABI-changing annotations.
This is not a host JVM, `System.loadLibrary` implementation, automatic
`JNI_OnLoad` call or execution of the Android application bootstrap.
Contracts follow the [JNI function specification](https://docs.oracle.com/en/java/javase/17/docs/specs/jni/functions.html)
and [invocation API](https://docs.oracle.com/en/java/javase/17/docs/specs/jni/invocation.html).

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

## Java library loading and DEX/native continuation

`android.java_library` explicitly binds the currently mapped, initialized ELF
image to one logical `System.loadLibrary` name in the supplied DEX namespace:

```json
{"android":{"jni":true,"initialize":true,"java_library":"example",
            "dex_images":["<explicit DEX bytes>"]}}
```

The name is a nonempty ASCII identifier of at most 255 bytes, using letters,
digits, `_`, `-` and `.`. The input requires JNI and ELF initialization. It is
an analysis binding, not evidence of an APK, a native search path, or the
actual application class loader. No host file or shared library is opened.
The existing `libraries` option remains the catalogue of modeled system APIs;
it cannot supply an original `JNI_OnLoad` body.

An actual DEX call to `System.loadLibrary(String)` suspends its Java activation.
For the explicitly bound name, NeverD calls the ELF's original exported
`JNI_OnLoad(JavaVM *, null)` after its ELF initializers. The callback uses the
existing guest scheduler, instruction/deadline budget and implicit JNI local
frame. `FindClass` sees the one supplied DEX namespace. A valid API 28 result
(1.2, 1.4 or 1.6) completes the load. An ELF without that export completes without
a callback. Repeated loads do not rerun it; same-thread recursive loads can
continue. Invalid returned versions retain the failed load and throw
`UnsatisfiedLinkError`; already performed native writes and registrations remain.
Pending Java exceptions retain their heap identity, and class-initializer
failure uses Runtime's ordinary erroneous-class rules.

DEX invocations of registered ordinary native methods now use the same JNI
argument transport as JNI method calls. After guest return, the retained Java
registers, original invoke position, constructor state, exception handler and
class-initialization stack continue. No instructions or prior effects are
replayed. Native-to-Java callbacks can themselves suspend into another native
call. JNI method/field lookup and allocation finish their original operation
after a suspended initializer; reference returns are converted to caller-local
JNI handles only when that operation completes. Native and on-load local frames
retire even if an explicit nested frame was left open.

The standalone `dalvik::Runtime` enables suspension with a nonzero owner ID.
`ExecutionStop::Suspended` carries a request and a single-use continuation token;
`resume` accepts shared-heap values or exceptions for that exact occurrence.
Tokens are owner-bound and nested invocations complete in stack order. The
instruction closure is retained, and nested entries share the remaining stack
allowance. Synchronous callers still refuse native execution. At most 64
invocations may be retained; a terminal non-Java stop cannot be retried.

This is one explicit guest ELF and one supplied DEX namespace. Unknown library
bindings, native symbol-name lookup, custom/delegating loaders, unloading,
boot-loader native libraries, floating/stack native arguments, monitors and
cross-thread class or library initialization waits remain explicit stops.
Direct JNI calls to `System.loadLibrary` lack a DEX caller and remain unsupported.
This contract does not execute Android framework Application creation.

Behavior was checked against API 28 ART
[`JavaVMExt::LoadNativeLibrary`](https://android.googlesource.com/platform/art/+/android-9.0.0_r1/runtime/java_vm_ext.cc)
and the [JNI invocation specification](https://docs.oracle.com/javase/8/docs/technotes/guides/jni/spec/invocation.html#JNI_OnLoad).
Implementation and synthetic programs are independently authored.

Package query flags, version codes and target SDK fields use the shared signed
32-bit JSON integer contract, including decimal strings. Input budgets are
owned by the Dalvik limits header and reused by typed and JSON admission.

## ServiceManager cache and Binder state

The API 28 ServiceManager initializer uses shared system-property reads, heap
arrays and elapsed realtime. It requires those runtime services even when only
JNI method or field lookup triggers initialization. An Application phase does
not establish the current service cache or manager proxy.

The initializer constructs StatLogger, whose next tick is elapsed realtime in
milliseconds plus one second. In the native profile this reads the explicit
`linux_time.clocks` entry with `id: 7` (`CLOCK_BOOTTIME`) through the same
LinuxClock used by native `clock_gettime`. Supplying realtime, monotonic or
monotonic raw does not provide that observation. A missing boot clock stops
initialization before the logger is published; successful initialization keeps
one logger and its original next tick across later active uses and idle waits.

The boot epoch is independent of the monotonic epoch. A supplied difference can
represent prior suspension, but the model does not infer it. Declared wall
clocks share instruction/idle advancement; there is no additional simulated
system-suspend transition. Keep hypothetical boot epochs explicitly labeled as
synthetic scenario inputs, separate from device observations. Supplying a clock
does not establish service-cache contents or Binder state.

The optional `application_snapshot.service_manager` object has four members:

- `binder_proxies`: an array of `{ "key": 7, "warn_on_blocking": true }` records.
  Keys are unique positive uint32 values identifying Java proxy objects only.
  The optional warning flag is unknown when omitted. Keys are not kernel Binder
  handles and never provide native peers, liveness or transaction responses.
- `cache`: the complete initial array of `{ "name": "example", "binder": 7 }`
  entries. Names can be String or null; Binder values can be a declared proxy
  key or null. Names must be unique by Java UTF-16 value. An omitted cache is
  unknown; `[]` is explicitly empty. A null value is a cache miss for lookup.
- `manager_proxy`: a declared Binder key for the initial ServiceManagerProxy,
  null for known uninitialized state, or omitted for unknown state.
- `context_object`: the independently supplied BinderInternal context-object
  result, as a declared Binder key, null, or omitted for unknown state.

Limits are 256 cache entries, 256 proxy identities and 4096 UTF-8 bytes per
service name. Typed inputs and JSON share validation. Referencing the same proxy
key retains the same Java object across cached services and manager references.

Cached lookups and `initServiceCache` operate on the shared mutable HashMap.
An absent snapshot is never converted into a successful empty cache lookup.
An empty cache or a null-valued entry establishes a cache miss, not service
absence. Both `getService` and `checkService` then require manager/context
observations; supplying a context object does not establish manager state.
A nonnull cached proxy can be returned while manager, context and warning
state remain unknown. The hit preserves its warning state and does not start
another StatLogger clock read. JNI normal, varargs and array calls use the
same retained object. Guest mutations of `sCache` affect subsequent lookups.

For synthetic scenarios, declare the complete assumed cache and its referenced
Java proxy keys explicitly. A hypothetical cached proxy establishes neither a
device service observation nor a native Binder peer or callable remote service.
Cache misses require explicit manager/context state and stop before unmodeled
Binder transactions; local Binder objects, native object translation and RPC
responses remain outside this subset. With `observe_java_state`, the optional
`java_state.framework.service_manager` report retains cache completeness,
entry identities, manager/logger identities and proxy warning state. Report
heap identities use hexadecimal strings; input proxy keys remain numeric keys.
