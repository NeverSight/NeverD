import fixture.Core;
import fixture.Provider;
import fixture.Special;
import fixture.Trace;
import fixture.Failure;

public final class InstanceReifiableResultHarness {
    private static void check(boolean value, String detail) {
        if (!value) throw new AssertionError("owned instance reifiable result mismatch: " + detail);
    }
    private static void attempt(int route, final int mode, int attempt) {
        final Object first = new Object(), second = new Object(), marker = new Object(), replacement = new Object();
        final Provider actual = attempt == 0 ? new Provider(marker) : new Special(marker);
        final Provider receiver = mode == 2 ? null : actual;
        final String name = mode == 1 ? null : mode == 17 ? "" : new String("owned");
        final Object initial = mode == 3 ? "value" : mode == 4 ? null : mode == 5 ? new Object[]{first}
                : mode == 6 ? new String[]{"value"} : mode == 7 ? String.class : mode == 8 ? new int[]{1} : mode == 16 ? actual : first;
        final Object after = mode == 14 ? second : initial;
        final Failure failure = new Failure();
        final RuntimeException other = new IllegalStateException("owned runtime");
        final Error error = new LinkageError("owned error");
        final int[] callbacks = {0};
        Trace.value = initial;
        Trace.marker = marker;
        Trace.observedMarker = null;
        Trace.receiver = Trace.specialReceiver = null;
        Trace.defaultReceiver = receiver;
        Trace.caughtFailure = null;
        Trace.name = null;
        Trace.key = name;
        Trace.calls = Trace.overrides = Trace.caught = Trace.stage = 0;
        Trace.length = -1;
        Trace.action = mode == 13 ? null : () -> {
            callbacks[0]++;
            check(Trace.calls == 1 && Trace.stage == 2 && Trace.caught == 0, "getter before caller catch");
            check(Trace.receiver == receiver && Trace.observedMarker == marker, "receiver and marker snapshot");
            check(Trace.name == name && Trace.length == name.length(), "key snapshot");
            check(Trace.overrides == (actual instanceof Special ? 1 : 0), "virtual dispatch before getter");
            if (mode == 9) throw failure;
            if (mode == 10) throw other;
            if (mode == 11) throw error;
            if (mode == 12) throw (RuntimeException)null;
            if (mode == 14) Trace.value = after;
            if (mode == 15) { Trace.key = "changed"; Trace.defaultReceiver = new Provider(replacement); Trace.marker = replacement; }
        };
        Object value = null;
        Throwable thrown = null;
        try {
            switch (route) {
                case 0: value = Core.direct(receiver,name); break;
                case 1: value = Core.narrow(receiver,name); break;
                case 2: value = Core.quiet(receiver,name); break;
                case 3: value = Core.quietNarrow(receiver,name); break;
                case 4: value = Core.array(receiver,name); break;
                case 5: value = Core.ignored(receiver,name); break;
                case 6: value = Core.noKey(receiver); break;
                case 7: value = Core.classValue(receiver,name); break;
                default: throw new AssertionError("owned route");
            }
        } catch (Throwable caught) { thrown = caught; }
        boolean noReceiver = mode == 2;
        boolean nullFailure = mode == 1 || mode == 12 || mode == 13;
        boolean innerFailure = nullFailure || mode == 9 || mode == 10 || mode == 11;
        boolean quiet = route == 2 || route == 3;
        boolean handled = !noReceiver && quiet && innerFailure && mode != 11;
        Object expected = handled ? null : after;
        boolean badCast = !noReceiver && !innerFailure && expected != null &&
                ((route == 1 || route == 3) && !(expected instanceof String)
                 || route == 4 && !(expected instanceof Object[]) || route == 7 && !(expected instanceof Class));
        if (noReceiver || !handled && nullFailure) check(thrown instanceof NullPointerException, "null failure boundary");
        else if (mode == 9 && !handled) check(thrown == failure, "declared runtime identity");
        else if (mode == 10 && !handled) check(thrown == other, "other runtime identity");
        else if (mode == 11) check(thrown == error, "Error outside Exception catch");
        else if (badCast) check(thrown instanceof ClassCastException, "caller cast after getter");
        else check(thrown == null && value == (route == 5 ? mode == 15 ? replacement : marker : expected), "actual opaque result");
        check(Trace.calls == (noReceiver ? 0 : 1) && callbacks[0] == (noReceiver || mode == 1 || mode == 13 ? 0 : 1), "one getter and callback");
        check(Trace.caught == (handled ? 1 : 0), "exact caller catch");
        if (!handled) check(Trace.caughtFailure == null, "no invented caught object");
        else if (mode == 9) check(Trace.caughtFailure == failure, "caught declared runtime identity");
        else if (mode == 10) check(Trace.caughtFailure == other, "caught other runtime identity");
        else check(Trace.caughtFailure instanceof NullPointerException, "caught original null failure");
        check(Trace.overrides == (!noReceiver && actual instanceof Special ? 1 : 0), "override count");
        check(Trace.specialReceiver == (!noReceiver && actual instanceof Special ? actual : null), "override receiver");
        check(Trace.receiver == (noReceiver ? null : receiver) && Trace.observedMarker == (noReceiver ? null : marker), "original receiver fields");
        check(Trace.name == (noReceiver ? null : name), "original key reference");
        check(Trace.length == (noReceiver || mode == 1 ? -1 : name.length()), "key length before callback");
        check(Trace.stage == (noReceiver ? 0 : mode == 1 ? 1 : innerFailure ? 2 : 3), "read, callback and caller order");
    }
    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; attempt++) attempt(route,mode,attempt);
        System.out.println("attempts=2,instance-reifiable-result=matched");
    }
}
