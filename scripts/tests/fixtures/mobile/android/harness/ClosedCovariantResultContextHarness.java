import fixture.Core;
import fixture.Trace;

public final class ClosedCovariantResultContextHarness {
    private static void require(boolean value) {
        if (!value) throw new AssertionError("owned closed covariant result context mismatch");
    }
    private static Object call(int route, Object receiver, Class<? extends java.lang.annotation.Annotation> type, Object[] values) {
        switch (route) {
            case 0: return Core.raw(receiver, type, values);
            case 1: return Core.narrow(receiver, type, values);
            case 2: Core.ignore(receiver, type, values); return null;
            case 3: return Core.relay(receiver, type, values);
            case 4: return Core.fixed(receiver, type, values);
            default: throw new AssertionError();
        }
    }
    private static void check(int route, final int mode) throws Exception {
        final Object receiver = mode == 3 ? null : new String("receiver");
        final Class<? extends java.lang.annotation.Annotation> type = mode == 4 ? null : Deprecated.class;
        final Class<? extends java.lang.annotation.Annotation> changedType = Override.class;
        final Object first = new String("first");
        final Object[] values = mode == 5 ? null : mode == 6 ? new Object[0]
            : mode == 15 ? new String[] {(String)first} : new Object[] {first};
        final Object poison = new Object();
        final Object[] changed = new Object[] {poison};
        final String late = new String("late");
        final RuntimeException runtime = new IllegalArgumentException("callback");
        final Error error = new AssertionError("callback");
        final int[] callbacks = {0};
        Trace.calls = Trace.relays = Trace.stage = 0;
        Trace.receiver = Trace.first = null;
        Trace.type = null; Trace.values = null;
        Trace.value = mode == 1 ? new Object() : mode == 2 ? null
            : mode == 14 ? receiver : new String("result");
        Object expected = Trace.value;
        final Runnable action = new Runnable() {
            public void run() {
                callbacks[0]++;
                require(Trace.stage == 2 && Trace.receiver == receiver && Trace.type == type
                    && Trace.values == values && Trace.first == first);
                if (mode == 8) throw runtime;
                if (mode == 9) throw error;
                if (mode == 10) Trace.value = late;
                if (mode == 11) Trace.value = poison;
                if (mode == 12) values[0] = poison;
                if (mode == 13) {
                    Trace.receiver = poison; Trace.type = changedType;
                    Trace.values = changed; Trace.action = null;
                }
                if (mode == 15) values[0] = poison;
            }
        };
        Trace.action = mode == 7 ? null : action;
        if (mode == 10) expected = late;
        if (mode == 11) expected = poison;
        Object result = null; Throwable failure = null;
        try { result = call(route, receiver, type, values); }
        catch (Throwable caught) { failure = caught; }
        require(Trace.calls == 1 && Trace.relays == (route == 4 ? 1 : 0));
        require(Trace.receiver == (mode == 13 ? poison : receiver));
        require(Trace.type == (mode == 13 ? changedType : type));
        require(Trace.values == (mode == 13 ? changed : values));
        require(Trace.first == (mode == 5 || mode == 6 ? null : first));
        require(Trace.action == (mode == 7 || mode == 13 ? null : action));
        int stage = mode == 5 || mode == 6 ? 1
            : mode == 7 || mode == 8 || mode == 9 || mode == 15 ? 2 : 3;
        require(Trace.stage == stage);
        require(callbacks[0] == (mode == 5 || mode == 6 || mode == 7 ? 0 : 1));
        if (mode == 5 || mode == 7) require(failure instanceof NullPointerException);
        else if (mode == 6) require(failure instanceof ArrayIndexOutOfBoundsException);
        else if (mode == 8) require(failure == runtime);
        else if (mode == 9) require(failure == error);
        else if (mode == 15) require(failure instanceof ArrayStoreException);
        else if (route == 1 && (mode == 1 || mode == 11)) require(failure instanceof ClassCastException);
        else require(failure == null && result == (route == 2 ? null : expected));
        if (mode == 12) require(values[0] == poison);
    }
    public static void main(String[] args) throws Exception {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        check(route, mode); check(route, mode);
        System.out.println("attempts=2,closed-covariant-result-context=matched");
    }
}
