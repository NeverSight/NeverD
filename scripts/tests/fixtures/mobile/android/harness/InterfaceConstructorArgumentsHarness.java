import fixture.Api;
import fixture.Tile;
import fixture.Trace;

public final class InterfaceConstructorArgumentsHarness {
    private static final RuntimeException RUNTIME = new RuntimeException("owned construction failure");
    private static final Error ERROR = new AssertionError("owned construction error");

    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned interface constructor arguments mismatch");
    }

    private static int events(int constructors) {
        int value = 1;
        for (int i = 0; i < constructors; i++) value = value * 10 + 2;
        return value;
    }

    private static Object read(int route, Class<?> api) throws Throwable {
        if (route == 0) return Api.Z_FIRST;
        if (route == 1) return Api.A_SECOND;
        if (route == 2) return Api.B_FLOAT;
        return api.getField("M_THIRD").get(null);
    }

    private static boolean flag(int slot) { return slot == 0 || slot == 2; }
    private static int number(int slot) {
        return slot == 0 ? -17 : slot == 1 || slot == 3 ? Integer.MIN_VALUE : 0;
    }
    private static long ticks(int slot) {
        return slot == 0 ? 0x0123456789ABCDEFL : slot == 1 ? Long.MIN_VALUE
                : slot == 3 ? 0x7ff8000000000000L : 0;
    }

    public static void main(String[] args) throws Throwable {
        final int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        final int failedConstructor = mode == 3 || mode == 4 ? 1 : mode == 5 || mode == 6 || mode == 10 ? 2
                : mode == 7 || mode == 8 ? 3 : mode == 12 || mode == 13 ? 4 : 0;
        final boolean bootFailure = mode == 1 || mode == 2 || mode == 9 || mode == 14;
        final boolean failureExpected = bootFailure || failedConstructor != 0;
        final Tile[] constructed = new Tile[4];
        final Object marker = new Object();
        final int[] callbacks = new int[1];
        Trace.events = Trace.stage = Trace.constructors = Trace.completed = Trace.alternates = 0;
        Trace.escaped = null;
        Trace.flag = false;
        Trace.number = 123;
        Trace.ticks = -123;
        Trace.action = mode == 9 ? null : new Runnable() {
            public void run() {
                callbacks[0]++;
                int count = Trace.constructors;
                require(Trace.events == events(count) && Trace.alternates == 0);
                require(Api.Z_FIRST == (count > 1 ? constructed[0] : null));
                require(Api.A_SECOND == (count > 2 ? constructed[1] : null));
                require(Api.M_THIRD == (count > 3 ? constructed[2] : null));
                require(Api.B_FLOAT == null);
                if (Trace.stage == 1) {
                    require(count == 0 && Trace.completed == 0 && Trace.escaped == null);
                    if (mode == 1) throw RUNTIME;
                    if (mode == 2) throw ERROR;
                    if (mode == 14) throw (RuntimeException)null;
                    return;
                }
                require(count >= 1 && count <= 4 && Trace.stage == 10 + count);
                require(Trace.completed == count - 1 && Trace.escaped != null);
                Tile value = Trace.escaped;
                require(!value.flag && value.number == 0 && value.ticks == 0 && value.mark == null);
                for (int i = 0; i < count - 1; i++) require(value != constructed[i]);
                constructed[count - 1] = value;
                require(Trace.flag == flag(count - 1) && Trace.number == number(count - 1)
                        && Trace.ticks == ticks(count - 1));
                if (mode == 11) value.mark = marker;
                if (count == failedConstructor) {
                    if (mode == 4 || mode == 6 || mode == 8 || mode == 13) throw ERROR;
                    if (mode == 10) throw (RuntimeException)null;
                    throw RUNTIME;
                }
            }
        };
        Class<?> api = Class.forName("fixture.Api", false, InterfaceConstructorArgumentsHarness.class.getClassLoader());
        require(Trace.events == 0 && Trace.stage == 0 && callbacks[0] == 0);
        for (int attempt = 0; attempt < 2; attempt++) {
            Object result = null;
            Throwable failure = null;
            try { result = read(route, api); }
            catch (Throwable caught) { failure = caught; }
            int count = bootFailure ? 0 : failedConstructor != 0 ? failedConstructor : 4;
            int completed = failureExpected ? Math.max(0, count - 1) : 4;
            require(Trace.constructors == count && Trace.completed == completed);
            require(Trace.events == events(count) && Trace.stage == (bootFailure ? 1 : 10 + count));
            require(callbacks[0] == (mode == 9 ? 0 : 1 + count));
            if (failureExpected) {
                if (attempt != 0) require(failure instanceof NoClassDefFoundError);
                else if (mode == 2 || mode == 4 || mode == 6 || mode == 8 || mode == 13) require(failure == ERROR);
                else {
                    require(failure instanceof ExceptionInInitializerError);
                    if (mode == 9 || mode == 10 || mode == 14) require(failure.getCause() instanceof NullPointerException);
                    else require(failure.getCause() == RUNTIME);
                }
            } else {
                require(failure == null && result == constructed[route == 0 ? 0 : route == 1 ? 1 : route == 2 ? 3 : 2]);
                require(Api.Z_FIRST == constructed[0] && Api.A_SECOND == constructed[1]
                        && Api.M_THIRD == constructed[2] && Api.B_FLOAT == constructed[3]);
            }
            for (int i = 0; i < count; i++) {
                Tile value = constructed[i];
                require(value != null);
                if (i < completed)
                    require(value.flag == flag(i) && value.number == number(i) && value.ticks == ticks(i));
                else require(!value.flag && value.number == 0 && value.ticks == 0);
                require(value.mark == (mode == 11 ? marker : null));
            }
            if (attempt == 0) {
                require(Trace.alternates == 0);
                try {
                    Tile extra = new Tile(9);
                    require(!bootFailure && !extra.flag && extra.number == 9 && extra.ticks == 0 && extra.mark == null);
                } catch (Throwable caught) { require(bootFailure && caught instanceof NoClassDefFoundError); }
                if (mode == 15) Trace.action = new Runnable() { public void run() { throw ERROR; } };
            }
            require(Trace.alternates == (bootFailure ? 0 : 1));
        }
        System.out.println("attempts=2,interface-constructor-arguments=matched");
    }
}
