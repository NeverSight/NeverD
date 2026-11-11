import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class UnusedFormalHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("unused formal invocation changed"); }
    static Object call(Object input, int route) throws Throwable {
        if (route == 0) return Client.call(input);
        if (route == 1) return UnusedFormalCaller.call(input);
        if (route == 2) {
            try { return Client.class.getMethod("call", Object.class).invoke(null, input); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Object) MethodHandles.publicLookup().findStatic(Client.class, "call",
                MethodType.methodType(Object.class, Object.class)).invokeExact(input);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        require(Provider.class.getTypeParameters().length == 1);
        require(Provider.class.getMethod("relay", Object.class).getTypeParameters().length == 1);
        require(Provider.class.getTypeParameters()[0].getName().equals("T"));
        require(Provider.class.getMethod("relay", Object.class).getTypeParameters()[0].getName().equals("T"));
        Trace.failAt = mode >= 1 && mode <= 3 ? mode : 0;
        Trace.failure = new RuntimeException("owned unused formal failure");
        int[] values = {7}, output = {31};
        Object input = mode == 4 ? null : mode == 6 ? new Object() : values;
        Trace.result = mode == 5 ? null : mode == 7 ? new Object() : mode == 8 ? input : output;
        Object sentinel = new Object();
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.seen = sentinel; Trace.after = sentinel;
            int before = values[0]; Throwable failure = null; Object result = null;
            try { result = call(input, route); } catch (Throwable error) { failure = error; }
            boolean read = mode != 1 && mode != 2;
            boolean finished = read && mode != 6;
            int trace = attempt == 0 ? 1 : 0;
            if (mode != 1) trace = trace * 10 + 2;
            if (finished) trace = trace * 10 + 3;
            require(Trace.events == trace && Trace.seen == (read ? input : sentinel));
            require(values[0] == before + (finished && mode != 4 ? 1 : 0));
            if (mode == 1) {
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (mode == 2 || mode == 3) require(failure == Trace.failure);
            else if (mode == 6) require(failure instanceof ClassCastException);
            else {
                require(failure == null && result == Trace.result && Trace.after == result);
                if (mode == 0) { output[0]++; require(((int[]) result)[0] == output[0]); }
            }
            if (failure != null) require(Trace.after == sentinel);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
