import fixture.Child;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;

public final class ConstructorClassLiteralHarness {
    static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    static Child create(int route, Object value) throws Throwable {
        if (route == 0) return new Child(value);
        if (route == 1) return ConstructorClassLiteralCaller.create(value);
        Class<?> type = Class.forName("fixture.Child", false,
                ConstructorClassLiteralHarness.class.getClassLoader());
        if (route == 2) {
            try { return (Child) type.getConstructor(Object.class).newInstance(value); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Child) MethodHandles.publicLookup().findConstructor(type,
                MethodType.methodType(void.class, Object.class)).invoke(value);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Object input = mode == 1 ? null : new Object();
        Trace.failure = new RuntimeException("owned class-literal failure");
        Trace.failAt = mode == 2 ? 2 : mode == 3 ? 3 : mode == 4 ? 5 :
                mode == 5 ? 6 : mode == 7 ? 1 : mode == 8 ? 7 : 0;
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.trace = 0;
            int expected = 0;
            boolean failedOwner = mode == 7 || mode == 8;
            int[] steps = attempt == 0 ? new int[]{1,7,2,3,4,5,6} : new int[]{2,3,4,5};
            if (!(failedOwner && attempt == 1)) {
                for (int step : steps) {
                    if (mode == 6 && step == 2) break;
                    expected = expected * 10 + step;
                    if (step == Trace.failAt) break;
                }
            }
            Throwable error = null;
            Child result = null;
            try {
                result = create(route, input);
                int beforeInitialization = attempt == 0 ? 172345 : 2345;
                require(Trace.trace == beforeInitialization, "class literal initialized its target early");
                Class<?> token = Class.forName("fixture.Probe", false,
                        ConstructorClassLiteralHarness.class.getClassLoader());
                require(result.type == token && result.observed == token, "class literal identity changed");
                require(result.input == input && result.other == input && Trace.seen == input,
                        "original reference argument changed");
                require(result.prepared == (input == null ? -13 : 29), "prepared argument changed");
                require(result.bits == 0x8000000000000007L &&
                        Double.doubleToRawLongBits(result.number) == Long.MIN_VALUE, "wide argument bits changed");
                result.observed = String.class;
                require(result.type == token && result.observed == String.class, "mutable class alias changed");
                Class.forName("fixture.Probe", true, ConstructorClassLiteralHarness.class.getClassLoader());
            } catch (Throwable failure) { error = failure; }
            require(Trace.trace == expected, "class resolution or constructor failure order changed");
            if (mode <= 1) require(error == null && result != null, "successful construction failed");
            else if (mode == 6) {
                require(error instanceof NoClassDefFoundError && result == null,
                        "missing literal class did not fail before construction");
            } else if (mode == 5 || failedOwner) {
                if (attempt == 0) require(error instanceof ExceptionInInitializerError &&
                        error.getCause() == Trace.failure, "initialization exception identity changed");
                else require(error instanceof NoClassDefFoundError, "failed initialization was retried");
                require(mode != 5 || result != null, "erroneous class literal failed before explicit initialization");
            } else require(error == Trace.failure && result == null, "original constructor exception changed");
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.trace);
        }
    }
}
