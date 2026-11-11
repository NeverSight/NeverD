import fixture.Core;
import fixture.Failure;
import fixture.Trace;

public final class GuardedConstructorUncheckedGetterHarness {
    private static Object[] previousLiteral;
    private static void check(boolean value, String detail) {
        if (!value) throw new AssertionError("owned guarded constructor unchecked getter mismatch: " + detail);
    }
    private static void attempt(int route, final int mode, int attempt) {
        final boolean externalArray = route == 2 || route == 3 || route == 5;
        final boolean direct = route == 1 || route == 3 || route == 5;
        final boolean ignored = route >= 4;
        final Class<?> type = mode == 1 ? null : attempt == 0 ? String.class : Object.class;
        final Object first = new Object(), second = new Object(), marker = new Object(), replacementMarker = new Object();
        final Object[] arguments = mode == 14 ? null : mode == 15 ? new String[]{"owned"} : new Object[]{first,second};
        final Object initial = mode == 2 ? null : mode == 3 ? Integer.class : mode == 4 ? new String[]{"owned"} : first;
        final Object after = mode == 10 ? second : initial;
        final Failure failure = new Failure();
        final RuntimeException other = new IllegalStateException("owned runtime");
        final Error error = new LinkageError("owned error");
        final int[] callbacks = {0};
        Trace.value = initial;
        Trace.type = Trace.baseValue = null;
        Trace.arguments = null;
        Trace.name = null;
        Trace.marker = marker;
        Trace.calls = Trace.bases = Trace.objects = Trace.subjects = Trace.directs = Trace.stage = 0;
        Trace.arity = -1;
        Trace.action = mode == 9 ? null : () -> {
            callbacks[0]++;
            check(Trace.stage == 2 && Trace.type == type && Trace.name.equals(type.getName()), "token snapshot");
            check(Trace.bases == 0 && Trace.objects == 0 && Trace.subjects == 0 && Trace.directs == 0, "getter before delegation");
            if (externalArray) check(Trace.arguments == arguments && Trace.arity == arguments.length, "original array reference");
            else {
                check(Trace.arguments != null && Trace.arguments.length == 0 && Trace.arity == 0, "literal array");
                if (attempt != 0) check(Trace.arguments != previousLiteral, "fresh literal allocation");
                previousLiteral = Trace.arguments;
            }
            if (mode == 5) throw failure;
            if (mode == 6) throw other;
            if (mode == 7) throw error;
            if (mode == 8) throw (RuntimeException)null;
            if (mode == 10) Trace.value = after;
            if (mode == 11) Trace.type = Integer.class;
            if (mode == 12) Trace.arguments = new Object[]{second};
            if (mode == 13) Trace.marker = replacementMarker;
        };
        Object value = null;
        Throwable thrown = null;
        try {
            switch (route) {
                case 0: value = Core.local(type); break;
                case 1: value = Core.direct(type); break;
                case 2: value = Core.localArray(type,arguments); break;
                case 3: value = Core.directArray(type,arguments); break;
                case 4: value = Core.ignored(type); break;
                case 5: value = Core.ignoredArray(type,arguments); break;
                default: throw new AssertionError("owned route");
            }
        } catch (Throwable caught) { thrown = caught; }
        boolean early = mode == 1 || (externalArray && mode == 14);
        boolean nullFailure = early || mode == 8 || mode == 9;
        boolean failureExpected = nullFailure || mode == 5 || mode == 6 || mode == 7;
        if (nullFailure) check(thrown instanceof NullPointerException, "exact null failure");
        else if (mode == 5) check(thrown == failure, "declared runtime identity");
        else if (mode == 6) check(thrown == other, "other runtime identity");
        else if (mode == 7) check(thrown == error, "error identity");
        else check(thrown == null && value == (ignored ? mode == 13 ? replacementMarker : marker : after), "actual result");
        check(Trace.calls == 1 && callbacks[0] == (early || mode == 9 ? 0 : 1), "one getter and callback");
        check(Trace.arity == (early ? -1 : externalArray ? arguments.length : 0), "argument length snapshot");
        check(Trace.stage == (early ? 1 : failureExpected ? 2 : !direct && externalArray ? 6 : 5), "failure/delegation order");
        check(Trace.bases == (failureExpected ? 0 : 1) && Trace.objects == (failureExpected || direct ? 0 : 1)
                && Trace.subjects == (failureExpected || direct || !externalArray ? 0 : 1) && Trace.directs == (failureExpected || !direct ? 0 : 1), "exact constructor roles");
        check(Trace.baseValue == (failureExpected ? null : after), "original final store");
        check(Trace.type == (mode == 11 ? Integer.class : type), "token identity and mutation");
        if (mode == 1) check(Trace.name == null, "null token before name");
        else check(Trace.name.equals(type.getName()), "name snapshot");
        if (externalArray && mode != 12) check(Trace.arguments == arguments, "array identity");
    }
    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; attempt++) attempt(route,mode,attempt);
        System.out.println("attempts=2,guarded-constructor-unchecked-getter=matched");
    }
}
