import fixture.*;
import java.util.*;
import java.lang.reflect.*;
public final class NestedAccessMarkerHarness {
    static final class FutureContext extends Context { }
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned closed access marker invariant");
    }
    private static Object field(Object value, String name) {
        try {
            Field field = value.getClass().getDeclaredField(name);
            field.setAccessible(true);
            return field.get(value);
        } catch (ReflectiveOperationException failure) { throw new AssertionError(failure); }
    }
    @SuppressWarnings("unchecked")
    private static Set<Integer> values(Object value) { return (Set<Integer>) field(value, "values"); }
    @SuppressWarnings({"rawtypes", "unchecked"})
    static Set<Integer> poison(Set values) { return values; }
    private static void initialize(int mode, RuntimeException runtime, Error error) {
        Throwable failure = null;
        try { require(Core.choose(0) == 11); } catch (Throwable caught) { failure = caught; }
        require(Context.initializations == 1);
        if (mode == 16) require(failure instanceof ExceptionInInitializerError && failure.getCause() == runtime);
        else if (mode == 17) require(failure == error);
        else require(failure == null && Core.choose(1) == 22);
        failure = null;
        try { require(Core.choose(0) == 11); } catch (Throwable caught) { failure = caught; }
        require(Context.initializations == 1);
        if (mode == 16 || mode == 17) require(failure instanceof NoClassDefFoundError);
        else require(failure == null);
    }
    public static void main(String[] args) throws Throwable {
        final int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        final boolean rebuilt = Boolean.parseBoolean(args[3]);
        final RuntimeException initRuntime = new IllegalStateException("owned initializer");
        final Error initError = new Error("owned initializer error");
        Context.initializer = new Runnable() { public void run() {
            if (mode == 16) throw initRuntime;
            if (mode == 17) throw initError;
        }};
        Class<?> markerType = Class.forName(args[2], false, NestedAccessMarkerHarness.class.getClassLoader());
        Class<?> entryType = Class.forName("fixture.Core$Entry", false, NestedAccessMarkerHarness.class.getClassLoader());
        require(entryType.getEnclosingClass() == Core.class && entryType.getEnclosingMethod() == null);
        require(Modifier.isPrivate(entryType.getModifiers()) && Modifier.isStatic(entryType.getModifiers()));
        require(Probe.class.isAssignableFrom(entryType));
        Constructor<?> access = entryType.getDeclaredConstructor(Set.class, Context.class, Probe.class, markerType);
        require(access.isSynthetic() != rebuilt && !Modifier.isPublic(access.getModifiers()) && !Modifier.isPrivate(access.getModifiers()));
        require(markerType.getEnclosingClass() == Core.class && markerType.getEnclosingMethod() == null && markerType.getDeclaredFields().length == 1);
        require(markerType.isAnonymousClass() != rebuilt && markerType.isMemberClass() == rebuilt);
        Field state = markerType.getDeclaredFields()[0];
        require(state.isSynthetic() && Modifier.isStatic(state.getModifiers()) && Modifier.isFinal(state.getModifiers()));
        require(markerType.getDeclaredConstructors().length == (rebuilt ? 1 : 0));
        if (rebuilt) require(Modifier.isPrivate(markerType.getDeclaredConstructors()[0].getModifiers()));
        access.setAccessible(true);
        require(Context.initializations == 0);
        if (route >= 2) initialize(mode, initRuntime, initError);
        for (int attempt = 0; attempt < 2; ++attempt) {
            final LinkedHashSet<Object> raw = new LinkedHashSet<Object>();
            if (mode != 11) raw.add(mode == 12 ? "poison" : Integer.valueOf(42));
            final Set<Integer> values = mode == 1 ? null : poison(raw);
            final Context context = mode == 2 ? null : mode == 13 ? new FutureContext() : new Context();
            final Object token = new Object(), changedToken = new Object();
            if (context != null) context.token = token;
            final Object marker = mode == 10 ? null : new Object();
            final RuntimeException callbackFailure = new RuntimeException(), targetFailure = new RuntimeException();
            final Error callbackError = new AssertionError("callback sentinel");
            final int[] calls = {0};
            if (context != null) context.failure = mode == 6 || mode == 8 || mode == 14 ? targetFailure : null;
            Core.latest = null;
            final Probe probe = mode == 3 ? null : new Probe() {
                public Object observe(Object self, Object actualValues, Context actualContext) {
                    require(++calls[0] == 1 && self == Core.latest && actualValues == values && actualContext == context);
                    require(values(self) == values && field(self, "context") == context && field(self, "probe") == this && ((Integer) field(self, "stage")).intValue() == 1 && field(self, "marker") == null);
                    if (mode == 4 || mode == 14) throw callbackFailure;
                    if (mode == 5) throw callbackError;
                    if (mode == 7) context.failure = targetFailure;
                    if (mode == 8) context.failure = null;
                    if (mode == 9) raw.add(Integer.valueOf(99));
                    if (mode == 15) context.token = changedToken;
                    return marker;
                }
            };
            final int beforeInitialization = Context.initializations;
            Object result = null; Throwable failure = null;
            try {
                if (route % 2 == 0) result = Core.Builder.create(values, context, probe);
                else {
                    try { result = access.newInstance(values, context, probe, null); }
                    catch (InvocationTargetException caught) { throw caught.getCause(); }
                }
            } catch (Throwable caught) { failure = caught; }
            require(Context.initializations == beforeInitialization);
            boolean callbackFailed = mode == 3 || mode == 4 || mode == 5 || mode == 14;
            boolean targetFailed = mode == 6 || mode == 7;
            boolean success = !callbackFailed && !targetFailed;
            int stage = callbackFailed ? 1 : targetFailed ? 2 : 3;
            Object partial = Core.latest;
            require(partial != null && values(partial) == values && field(partial, "context") == context && field(partial, "probe") == probe);
            require(((Integer) field(partial, "stage")).intValue() == stage && field(partial, "marker") == (callbackFailed ? null : marker));
            require(calls[0] == (mode == 3 ? 0 : 1) && result == (success ? partial : null));
            if (mode == 3) require(failure instanceof NullPointerException);
            else if (mode == 4 || mode == 14) require(failure == callbackFailure);
            else if (mode == 5) require(failure == callbackError);
            else if (targetFailed) require(failure == targetFailure);
            else require(failure == null);
            if (success && values != null && !values.isEmpty()) {
                if (mode == 12) {
                    boolean bad = false;
                    try { Integer ignored = values(result).iterator().next(); }
                    catch (ClassCastException caught) { bad = true; }
                    require(bad && ((Integer) field(partial, "stage")).intValue() == 3);
                } else require(values(result).iterator().next().intValue() == 42);
            }
            if (context != null) require(context.token == (mode == 15 ? changedToken : token));
            if (mode == 9) require(raw.size() == 2);
            if (route < 2 && attempt == 1) initialize(mode, initRuntime, initError);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",stage=" + stage);
        }
    }
}
