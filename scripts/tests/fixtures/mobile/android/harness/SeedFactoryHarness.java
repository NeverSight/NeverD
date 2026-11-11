import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class SeedFactoryHarness {
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("seed factory identity, construction or effects changed");
    }
    static Box<?> call(Object seed, long flags, int route) throws Throwable {
        if (route == 0) return Client.seed(seed, flags);
        if (route == 1) return SeedFactoryCaller.seed(seed, flags);
        if (route == 2) {
            try { return (Box<?>)Client.class.getMethod("seed", Object.class, long.class).invoke(null, seed, flags); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        return (Box<?>)MethodHandles.lookup().findStatic(Client.class, "seed",
                MethodType.methodType(Box.class, Object.class, long.class)).invokeExact(seed, flags);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Trace.failure = new RuntimeException("owned seed factory failure");
        Trace.failAt = mode == 3 ? 4 : mode == 4 ? 5 : mode == 1 || mode == 2 ? mode : 0;
        int[] mutable = {0};
        Object seed = mode == 5 ? null : mode == 6 ? new String("owned seed") : mode == 7 ? new Object() : mutable;
        Box<?> previous = null;
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0;
            int oldBoxes = Trace.boxes;
            long flags = attempt == 0 ? Long.MIN_VALUE : Long.MAX_VALUE;
            Box<?> result = null; Throwable failure = null;
            try { result = call(seed, flags, route); }
            catch (Throwable error) { failure = error; }
            String expected = mode == 1 ? "" : mode == 2 ? "2" : mode == 3 ? (attempt == 0 ? "24" : "2") : (attempt == 0 ? "245" : "25");
            if (attempt == 0) expected = "1" + expected;
            require(Trace.events == (expected.isEmpty() ? 0 : Long.parseLong(expected)));
            if (mode == 1 || mode == 3) {
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (mode == 2 || mode == 4) require(failure == Trace.failure);
            else {
                require(failure == null && result != previous && result.value == seed);
                if (seed == mutable) {
                    mutable[0]++;
                    require(((int[])result.value)[0] == mutable[0]);
                    if (previous != null) require(previous.value == result.value && ((int[])previous.value)[0] == mutable[0]);
                }
                previous = result;
            }
            require(Trace.boxes == oldBoxes + (mode != 1 && mode != 2 && mode != 3 ? 1 : 0));
            require(Trace.reads == 0 && Trace.flags == (mode == 1 ? 0 : flags));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
