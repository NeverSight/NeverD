import fixture.Carrier;
import fixture.Core;
import fixture.Trace;
import java.util.Arrays;
import java.util.List;
import java.util.Set;
import java.util.stream.Collector;

public final class NullaryNestedCarrierHarness {
    private static void check(boolean condition, String label) {
        if (!condition) throw new AssertionError("owned nullary nested carrier mismatch: " + label);
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    private static void attempt(final int route, final int mode, final int attempt) {
        final boolean library = route >= 4;
        final Object firstToken = mode == 1 || mode == 14 && attempt == 0
                ? new Object() : mode == 15 ? null : new String("first");
        final Object nextToken = new String("replacement");
        final Carrier first = new Carrier(firstToken);
        final Carrier next = new Carrier(nextToken);
        final Object invalid = new Object();
        final Object initial = mode == 2 ? null : mode == 9 ? invalid : first;
        final RuntimeException failure = new IllegalStateException("callback");
        final Error error = new LinkageError("callback");
        final RuntimeException initFailure = new IllegalArgumentException("initializer");
        final Runnable action = mode == 3 ? null : new Runnable() {
            public void run() {
                Trace.callbacks++;
                if (mode == 4) throw failure;
                if (mode == 5) throw error;
                if (mode == 6 || mode == 12) Trace.value = next;
                if (mode == 7) Trace.value = null;
                if (mode == 8) Trace.value = invalid;
                if (mode == 10) first.token = nextToken;
                if (mode == 12) throw failure;
                if (mode == 13) Trace.action = null;
            }
        };
        Trace.value = initial;
        Trace.action = action;
        Trace.initFailure = mode == 11 ? initFailure : null;
        Trace.calls = Trace.callbacks = Trace.stage = 0;
        final Object unwritten = new Object();
        Object result = unwritten;
        Throwable thrown = null;
        try {
            switch (route) {
            case 0: result = Core.raw(); break;
            case 1: result = Core.narrow(); break;
            case 2: Core.ignore(); break;
            case 3: result = Core.relay(); break;
            case 4: result = Core.setFactory(); break;
            case 5: result = Core.listFactory(); break;
            default: throw new AssertionError("route");
            }
        } catch (Throwable caught) {
            thrown = caught;
        }
        final boolean entered = !library && mode != 11;
        final boolean called = entered && mode != 3;
        final boolean returned = entered && mode != 3 && mode != 4 && mode != 5 && mode != 12;
        final Object value = called && (mode == 6 || mode == 12) ? next
                : called && mode == 7 ? null : called && mode == 8 ? invalid : initial;
        final Object token = called && mode == 10 ? nextToken : firstToken;
        final Class<?> failureType = library ? null : mode == 11
                ? attempt == 0 ? ExceptionInInitializerError.class : NoClassDefFoundError.class
                : mode == 3 ? NullPointerException.class : mode == 4 || mode == 12 ? IllegalStateException.class
                : mode == 5 ? LinkageError.class : value != null && !(value instanceof Carrier)
                ? ClassCastException.class : route == 1 && value == null ? NullPointerException.class
                : route == 1 && (value == next ? nextToken : token) != null
                    && !((value == next ? nextToken : token) instanceof String) ? ClassCastException.class : null;
        check(Trace.initializations == (library ? 0 : 1), "class initialization and erroneous reuse");
        check(Trace.calls == (entered ? 1 : 0) && Trace.callbacks == (called ? 1 : 0), "exact call count");
        check(Trace.stage == (returned ? 2 : entered ? 1 : 0), "effect order");
        check(Trace.value == value && first.token == token, "callback mutation and identity");
        check(Trace.action == (called && mode == 13 ? null : action), "callback replacement");
        check(failureType == null ? thrown == null : thrown != null && thrown.getClass() == failureType, "failure kind");
        if (!library && (mode == 4 || mode == 12)) check(thrown == failure, "exception identity");
        if (!library && mode == 5) check(thrown == error, "error identity");
        if (!library && mode == 11 && attempt == 0) check(thrown.getCause() == initFailure, "initializer cause");
        if (library) {
            check(result instanceof Collector, "actual standard factory result");
            Object a = new Object(), b = new Object();
            Object collected = Arrays.asList(a, b, a, null).stream().collect((Collector) result);
            if (route == 4) {
                check(collected instanceof Set, "set carrier");
                Set values = (Set) collected;
                check(values.size() == 3 && values.contains(a) && values.contains(b) && values.contains(null), "set contents");
            } else {
                check(collected instanceof List, "list carrier");
                List values = (List) collected;
                check(values.size() == 4 && values.get(0) == a && values.get(1) == b
                        && values.get(2) == a && values.get(3) == null, "list order and identity");
            }
        } else {
            Object expected = failureType != null || route == 2 ? unwritten
                    : route == 1 ? value == next ? nextToken : token : value;
            check(result == expected, "actual result and late cast");
        }
    }

    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; attempt++) attempt(route, mode, attempt);
        System.out.println("attempts=2,nullary-nested-carrier=matched");
    }
}
