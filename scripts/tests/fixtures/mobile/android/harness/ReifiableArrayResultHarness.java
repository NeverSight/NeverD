import fixture.Core;
import fixture.Trace;

public final class ReifiableArrayResultHarness {
    private static void require(boolean value) {
        if (!value) throw new AssertionError("owned reifiable array result mismatch");
    }
    private static Object call(int route, Object receiver, Class<?> type, Object[] values) {
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
        final Object receiver = mode == 3 ? null : new String[] {"receiver"};
        final Class<?> type = mode == 4 ? null : String.class;
        final Class<?> changedType = Object.class;
        final Object first = new String("first");
        final Object[] values = mode == 5 ? null : mode == 6 ? new Object[0]
            : mode == 15 ? new String[] {(String)first} : new Object[] {first};
        final Object poison = new Object();
        final Object[] changed = new Object[] {poison};
        final String[] late = new String[] {"late"};
        final RuntimeException runtime = new IllegalArgumentException("callback");
        final Error error = new AssertionError("callback");
        final int[] callbacks = {0};
        Trace.calls = Trace.relays = Trace.stage = 0;
        Trace.receiver = Trace.first = null;
        Trace.type = null; Trace.values = null;
        Trace.value = mode == 1 ? new Object[1] : mode == 2 ? null
            : mode == 14 ? receiver : new String[] {"result"};
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
        else if (mode == 11 || (route == 1 && mode == 1)) require(failure instanceof ClassCastException);
        else require(failure == null && result == (route == 2 ? null : expected));
        if (mode == 12) require(values[0] == poison);
    }

    private static Object previousAllocated;
    private static void checkAllocated(int route, final int mode) {
        final Class<?> type = mode == 1 ? Object.class : mode == 3 ? null
            : mode == 4 ? int.class : mode == 5 ? void.class
            : mode == 7 ? Integer.class : String.class;
        final int size = mode == 2 ? -1 : mode == 6 ? 0 : 2;
        final Object poison = new Object();
        final String value = new String("element");
        final RuntimeException runtime = new IllegalArgumentException("action");
        final Error error = new AssertionError("action");
        final int[] callbacks = {0};
        final Object[] observed = {null};
        Trace.calls = Trace.relays = Trace.stage = Trace.size = 0;
        Trace.type = null; Trace.allocated = null;
        final Runnable action = new Runnable() {
            public void run() {
                callbacks[0]++;
                require(Trace.stage == 2 && Trace.type == type && Trace.size == size);
                require(Trace.allocated != null && Trace.allocated.getClass().getComponentType() == type);
                require(java.lang.reflect.Array.getLength(Trace.allocated) == size);
                observed[0] = Trace.allocated;
                if (mode == 9) throw runtime;
                if (mode == 10) throw error;
                if (mode == 11) {
                    Trace.type = Object.class; Trace.size = -1;
                    Trace.allocated = poison; Trace.action = null;
                }
                if (mode == 12) ((Object[])Trace.allocated)[0] = poison;
                if (mode == 13) ((Object[])Trace.allocated)[0] = value;
            }
        };
        Trace.action = mode == 8 ? null : action;
        Object result = null; Throwable failure = null;
        try {
            if (route == 0) result = Core.allocate(type, size);
            else if (route == 1) result = Core.narrowAllocated(type, size);
            else Core.ignoreAllocated(type, size);
        } catch (Throwable caught) { failure = caught; }
        final boolean failedBeforeArray = mode == 2 || mode == 3 || mode == 5;
        final boolean failedInAction = mode == 8 || mode == 9 || mode == 10 || mode == 12;
        require(Trace.calls == 1 && Trace.relays == 0);
        require(Trace.stage == (failedBeforeArray ? 1 : failedInAction ? 2 : 3));
        require(Trace.type == (mode == 11 ? Object.class : type));
        require(Trace.size == (mode == 11 ? -1 : size));
        require(Trace.action == (mode == 8 || mode == 11 ? null : action));
        require(callbacks[0] == (failedBeforeArray || mode == 8 ? 0 : 1));
        Object allocated = mode == 11 ? observed[0] : Trace.allocated;
        if (failedBeforeArray) require(allocated == null);
        else {
            require(allocated != null && allocated != previousAllocated);
            previousAllocated = allocated;
            require(allocated.getClass().getComponentType() == type);
            require(java.lang.reflect.Array.getLength(allocated) == size);
        }
        if (mode == 2) require(failure instanceof NegativeArraySizeException);
        else if (mode == 3 || mode == 8) require(failure instanceof NullPointerException);
        else if (mode == 5) require(failure instanceof IllegalArgumentException);
        else if (mode == 9) require(failure == runtime);
        else if (mode == 10) require(failure == error);
        else if (mode == 12) require(failure instanceof ArrayStoreException);
        else if (mode == 4 || (route == 1 && (mode == 1 || mode == 7)))
            require(failure instanceof ClassCastException);
        else require(failure == null && result == (route == 2 ? null : allocated));
        if (mode == 11) require(Trace.allocated == poison);
        if (mode == 13) require(((Object[])allocated)[0] == value);
    }

    public static void main(String[] args) throws Exception {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        if (route < 5) { check(route, mode); check(route, mode); }
        else { checkAllocated(route - 5, mode); checkAllocated(route - 5, mode); }
        System.out.println("attempts=2,reifiable-array-result=matched");
    }
}
