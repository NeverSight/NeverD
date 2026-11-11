import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ContravariantCarrierHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("contravariant carrier changed"); }
    static Carrier<?> call(Carrier<?> input, int route) throws Throwable {
        if (route == 0) return Client.call(input);
        if (route == 1) return ContravariantCarrierCaller.call(input);
        if (route == 2) {
            try { return (Carrier<?>) Client.class.getMethod("call", Carrier.class).invoke(null, input); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Carrier<?>) MethodHandles.publicLookup().findStatic(Client.class, "call",
                MethodType.methodType(Carrier.class, Carrier.class)).invokeExact(input);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Trace.failAt = mode >= 1 && mode <= 3 ? mode : 0;
        Trace.failure = new RuntimeException("owned carrier failure");
        int[] before = {7}, seed = {19}, output = {31};
        Carrier<Object> input = mode == 4 ? null : new Carrier<Object>(before);
        Object initial = mode == 8 ? null : before;
        Trace.seed = mode == 7 ? null : seed;
        Trace.result = mode == 5 ? null : mode == 6 ? new Object() : mode == 9 ? input : new Carrier<Object>(output);
        Object sentinel = new Object();
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.seen = sentinel; Trace.after = sentinel;
            if (input != null) input.value = initial;
            Throwable failure = null; Carrier<?> result = null;
            try { result = call(input, route); } catch (Throwable error) { failure = error; }
            boolean mutation = mode != 1 && mode != 2 && mode != 4;
            int trace = attempt == 0 ? 1 : 0;
            if (mode != 1) trace = trace * 10 + 2;
            if (mutation) trace = trace * 10 + 3;
            require(Trace.events == trace && Trace.seen == (mutation ? initial : sentinel));
            if (input != null) require(input.value == (mutation ? Trace.seed : initial));
            if (mode == 1) {
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (mode == 2 || mode == 3) require(failure == Trace.failure);
            else if (mode == 4) require(failure instanceof NullPointerException);
            else if (mode == 6) require(failure instanceof ClassCastException);
            else {
                require(failure == null && result == Trace.result && Trace.after == result);
                if (mode != 5 && mode != 9) { output[0]++; require(((int[]) result.value)[0] == output[0]); }
                if (mode == 9) require(result.value == Trace.seed);
            }
            if (failure != null) require(Trace.after == sentinel);
            if (mutation && mode != 7) { seed[0]++; require(((int[]) input.value)[0] == seed[0]); }
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
