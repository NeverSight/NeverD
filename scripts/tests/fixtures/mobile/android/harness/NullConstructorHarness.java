import fixture.Child;
import fixture.Owner;
import fixture.Sink;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;

public final class NullConstructorHarness {
    static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    static Owner create(int variant, int route, int value) throws Throwable {
        if (route == 0) return variant == 0 ? new Owner(value) : new Child(value);
        if (route == 1) return NullConstructorCaller.create(variant, value);
        Class<?> type = Class.forName(variant == 0 ? "fixture.Owner" : "fixture.Child", false,
                NullConstructorHarness.class.getClassLoader());
        if (route == 2) {
            try { return (Owner) type.getConstructor(int.class).newInstance(value); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Owner) MethodHandles.publicLookup().findConstructor(type,
                MethodType.methodType(void.class, int.class)).invoke(value);
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]);
        int mode = Integer.parseInt(args[2]);
        int value = mode == 1 ? -23 : mode == 7 ? 0 : 17;
        Trace.failure = new RuntimeException("owned construction failure");
        Trace.failAt = mode == 2 ? 2 : mode == 3 ? 3 : mode == 4 ? 1 :
                mode == 5 && variant == 1 ? 5 : mode == 6 ? (variant == 0 ? 4 : 6) : 0;
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.trace = 0;
            int[] steps = attempt == 0 ? (variant == 0 ? new int[]{1,2,3,4} : new int[]{1,5,2,3,6}) :
                    (variant == 0 ? new int[]{2,3,4} : new int[]{2,3,6});
            boolean initFailure = Trace.failAt == 1 || Trace.failAt == 5;
            int expectedTrace = 0;
            if (!(initFailure && attempt == 1)) {
                for (int step : steps) {
                    expectedTrace = expectedTrace * 10 + step;
                    if (step == Trace.failAt) break;
                }
            }
            Owner result = null;
            Throwable error = null;
            try { result = create(variant, route, value); }
            catch (Throwable failure) { error = failure; }
            require(Trace.trace == expectedTrace, "constructor target or initialization order changed");
            if (Trace.failAt == 0) {
                require(error == null && result != null, "successful construction failed");
                require(result.tag == value && result.sink == null, "null or original argument changed");
                final int[] calls = {0};
                Sink<String> sink = text -> { require(text.equals("owned"), "sink value changed"); calls[0]++; };
                result.sink = sink;
                require(result.sink == sink, "mutable field identity changed");
                result.sink.accept("owned");
                result.tag++;
                require(calls[0] == 1 && result.tag == value + 1, "mutable object state changed");
            } else if (initFailure) {
                if (attempt == 0) require(error instanceof ExceptionInInitializerError &&
                        error.getCause() == Trace.failure, "initialization exception identity changed");
                else require(error instanceof NoClassDefFoundError, "failed initialization was retried");
            } else require(error == Trace.failure, "constructor exception identity changed");
            System.out.println("variant=" + variant + ",route=" + route + ",mode=" + mode +
                    ",attempt=" + attempt + ",trace=" + Trace.trace);
        }
    }
}
