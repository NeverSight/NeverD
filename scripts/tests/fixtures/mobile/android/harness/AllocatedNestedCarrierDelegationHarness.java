import fixture.Core;
import fixture.Pair;
import fixture.Trace;
import fixture.Wrap;
import java.util.HashMap;
import java.util.Map;

public final class AllocatedNestedCarrierDelegationHarness {
    private static void check(boolean condition, String label) {
        if (!condition) throw new AssertionError("owned allocated nested carrier mismatch: " + label);
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    private static void attempt(final int route, final int mode, int wide, int attempt) {
        final long tag = wide == 0 ? Long.MIN_VALUE + attempt : 0x12345678fedcba98L - attempt;
        final Wrap firstKey = new Wrap(), nextKey = new Wrap();
        final Pair firstValue = new Pair(), nextValue = new Pair();
        final Object invalid = new Object();
        final Object initial = mode == 1 ? invalid : mode == 2 ? null : firstValue;
        final RuntimeException failure = new IllegalStateException("callback");
        final Error error = new LinkageError("callback");
        final RuntimeException initFailure = new IllegalArgumentException("initializer");
        final Map given = new HashMap();
        given.put(firstKey, initial);
        final Runnable action = mode == 3 ? null : new Runnable() {
            public void run() {
                Trace.callbacks++;
                if (mode == 4) throw failure;
                if (mode == 5) throw error;
                if (mode == 6 || mode == 10) Trace.value = nextValue;
                if (mode == 7) Trace.value = invalid;
                if (mode == 8) Trace.value = null;
                if (mode == 9) { Trace.key = nextKey; Trace.tag = 7; }
                if (mode == 10) throw failure;
                if (mode == 12) Trace.action = null;
                if (mode == 13) ((Map) Trace.created).put(firstKey, invalid);
                if (mode == 15) { Trace.tag = 0; given.clear(); }
            }
        };
        Trace.key = firstKey;
        Trace.value = initial;
        Trace.tag = tag;
        Trace.action = action;
        Trace.initFailure = mode == 11 ? initFailure : null;
        Trace.created = Trace.partial = null;
        Trace.allocations = Trace.callbacks = Trace.parents = Trace.targets = Trace.stage = 0;
        Core<String, Integer> result = null;
        Throwable thrown = null;
        try {
            result = AllocatedNestedCarrierDelegationCaller.create(route, Trace.tag, mode == 14 ? null : given);
        } catch (Throwable caught) {
            thrown = caught;
        }
        final boolean allocated = route == 0 && mode != 11;
        final boolean called = allocated && mode != 3;
        final boolean completed = route != 0 || mode != 3 && mode != 4 && mode != 5 && mode != 10 && mode != 11;
        final Class<?> failureType = route != 0 ? null : mode == 11
                ? attempt == 0 ? ExceptionInInitializerError.class : NoClassDefFoundError.class
                : mode == 3 ? NullPointerException.class : mode == 4 || mode == 10 ? IllegalStateException.class
                : mode == 5 ? LinkageError.class : null;
        check(failureType == null ? thrown == null : thrown != null && thrown.getClass() == failureType, "constructor failure kind");
        if (route == 0 && (mode == 4 || mode == 10)) check(thrown == failure, "exception identity");
        if (route == 0 && mode == 5) check(thrown == error, "error identity");
        if (route == 0 && mode == 11 && attempt == 0) check(thrown.getCause() == initFailure, "initializer cause");
        check(Trace.initializations == (route == 0 ? 1 : 0), "class initialization and erroneous reuse");
        check(Trace.allocations == (allocated ? 1 : 0) && Trace.callbacks == (called ? 1 : 0), "allocation and callback count");
        check((Trace.created != null) == allocated, "partial allocation");
        check(Trace.parents == (completed ? 1 : 0) && Trace.targets == (completed ? 1 : 0), "constructor chain count");
        check(Trace.stage == (completed ? 4 : allocated ? 1 : 0), "effect order");
        check(Trace.partial == result && (result != null) == completed, "partial owner identity");
        final Object key = called && mode == 9 ? nextKey : firstKey;
        final Object value = called && (mode == 6 || mode == 10) ? nextValue
                : called && mode == 7 ? invalid : called && mode == 8 ? null : initial;
        check(Trace.key == key && Trace.value == value, "callback mutations");
        check(Trace.action == (called && mode == 12 ? null : action), "callback replacement");
        check(Trace.tag == (called && mode == 9 ? 7 : called && mode == 15 ? 0 : tag), "callback tag mutation");
        if (!completed) return;
        check(result.tag == tag, "original wide argument snapshot");
        if (route == 0) check(result.map == Trace.created && result.map.size() == 1, "allocated object and contents");
        if (route == 1) check(result.map == (mode == 14 ? null : given), "provided map identity");
        if (route == 2) check(result.map != null && result.map.getClass() == HashMap.class && result.map.isEmpty(), "original standard allocation");
        final Object expected = route == 0 ? value : route == 1 ? initial : null;
        final boolean absent = route == 1 && mode == 14;
        Object raw = null, typed = null;
        Throwable rawFailure = null, typedFailure = null;
        try { raw = AllocatedNestedCarrierDelegationCaller.raw(result, key); } catch (Throwable caught) { rawFailure = caught; }
        try { typed = AllocatedNestedCarrierDelegationCaller.typed(result, key); } catch (Throwable caught) { typedFailure = caught; }
        check(absent ? rawFailure instanceof NullPointerException : rawFailure == null && raw == expected, "raw contents and identity");
        if (absent) check(typedFailure instanceof NullPointerException, "null map failure");
        else if (expected != null && !(expected instanceof Pair)) check(typedFailure instanceof ClassCastException, "late element cast");
        else check(typedFailure == null && typed == expected, "typed contents and identity");
    }

    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]), wide = Integer.parseInt(args[2]);
        for (int attempt = 0; attempt < 2; attempt++) attempt(route, mode, wide, attempt);
        System.out.println("attempts=2,allocated-nested-carrier=matched");
    }
}
