import fixture.Core;
import fixture.Gate;
import fixture.Trace;

public final class ArrayConsumerArrayResultHarness {
    private static void require(boolean value) {
        if (!value) throw new AssertionError("owned array consumer array result mismatch");
    }
    @SuppressWarnings({"unchecked", "rawtypes"})
    private static Object call(int route, Object[] values, Gate<?> gate) {
        switch (route) {
            case 0: return Core.raw(values, gate);
            case 1: return Core.narrow(values, gate);
            case 2: Core.ignore(values, gate); return null;
            case 3: return Core.relay(values, (Gate)gate);
            case 4: return Core.reverse(values, gate);
            default: throw new AssertionError();
        }
    }
    private static void check(int route, final int mode) {
        final String first = new String("first");
        final String replaced = new String("replaced");
        final Object[] values = mode == 3 ? null : mode == 5 ? new String[0]
            : mode == 4 ? new Object[] {first} : new String[] {first};
        final Object poison = new Object();
        final Object[] changed = new Object[] {poison};
        final String[] late = new String[] {"late"};
        final RuntimeException gateRuntime = new IllegalArgumentException("gate");
        final Error gateError = new AssertionError("gate");
        final RuntimeException actionRuntime = new IllegalStateException("action");
        final Error actionError = new AssertionError("action");
        final int[] gates = {0}, actions = {0};
        final Gate<?>[] gateRef = new Gate<?>[1];
        gateRef[0] = new Gate<Object>() {
            public boolean accept(Object value) {
                gates[0]++;
                require(Trace.stage == 2 && Trace.input == values && Trace.gate == gateRef[0]);
                require(Trace.first == first && value == first);
                if (mode == 8) throw gateRuntime;
                if (mode == 9) throw gateError;
                if (mode == 10) throw null;
                if (mode == 11) Trace.value = late;
                if (mode == 12) values[0] = poison;
                if (mode == 13) { Trace.input = changed; Trace.gate = null; }
                if (mode == 14) values[0] = replaced;
                return mode != 15;
            }
        };
        final Gate<?> gate = mode == 4 ? null : gateRef[0];
        final Runnable action = new Runnable() {
            public void run() {
                actions[0]++;
                require(Trace.stage == 3 && Trace.first == first && Trace.accepted == (mode != 15));
                require(Trace.input == (mode == 13 ? changed : values));
                require(Trace.gate == (mode == 13 ? null : gateRef[0]));
                require(values[0] == (mode == 14 ? replaced : first));
                if (mode == 16) throw actionRuntime;
                if (mode == 17) throw actionError;
                if (mode == 19) Trace.value = late;
                if (mode == 20) Trace.value = poison;
                if (mode == 21) { Trace.input = changed; Trace.gate = null; Trace.action = null; }
                if (mode == 22) values[0] = poison;
                if (mode == 23) values[0] = replaced;
            }
        };
        Trace.calls = Trace.relays = Trace.stage = 0;
        Trace.input = null; Trace.gate = null; Trace.first = null;
        Trace.accepted = false; Trace.action = mode == 18 ? null : action;
        Trace.value = mode == 1 ? new Object[] {"result"} : mode == 2 ? null
            : mode == 6 ? values : mode == 7 ? poison : new String[] {"result"};
        Object expected = mode == 3 || mode == 4 ? values : mode == 11 || mode == 19 ? late : Trace.value;
        Object result = null; Throwable failure = null;
        try { result = call(route, values, gate); }
        catch (Throwable caught) { failure = caught; }
        final boolean shortcut = mode == 3 || mode == 4;
        final boolean gateFailure = mode == 8 || mode == 9 || mode == 10 || mode == 12;
        final boolean actionFailure = mode == 16 || mode == 17 || mode == 18 || mode == 22;
        require(Trace.calls == 1 && Trace.relays == (route == 4 ? 1 : 0));
        require(Trace.stage == (shortcut ? 5 : mode == 5 ? 1 : gateFailure ? 2 : actionFailure ? 3 : 4));
        require(Trace.input == (mode == 13 || mode == 21 ? changed : values));
        require(Trace.gate == (mode == 13 || mode == 21 ? null : gate));
        require(Trace.first == (shortcut || mode == 5 ? null : first));
        require(Trace.action == (mode == 18 || mode == 21 ? null : action));
        require(Trace.accepted == (!shortcut && mode != 5 && !gateFailure && mode != 15));
        require(gates[0] == (shortcut || mode == 5 ? 0 : 1));
        require(actions[0] == (shortcut || mode == 5 || gateFailure || mode == 18 ? 0 : 1));
        if (mode == 5) require(failure instanceof ArrayIndexOutOfBoundsException);
        else if (mode == 8) require(failure == gateRuntime);
        else if (mode == 9) require(failure == gateError);
        else if (mode == 10 || mode == 18) require(failure instanceof NullPointerException);
        else if (mode == 12 || mode == 22) require(failure instanceof ArrayStoreException);
        else if (mode == 16) require(failure == actionRuntime);
        else if (mode == 17) require(failure == actionError);
        else if (mode == 7 || mode == 20 || (route == 1 && (mode == 1 || mode == 4)))
            require(failure instanceof ClassCastException);
        else require(failure == null && result == (route == 2 ? null : expected));
        if (values != null && values.length != 0)
            require(values[0] == (mode == 14 || mode == 23 ? replaced : first));
    }
    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        check(route, mode); check(route, mode);
        System.out.println("attempts=2,array-consumer-array-result=matched");
    }
}
