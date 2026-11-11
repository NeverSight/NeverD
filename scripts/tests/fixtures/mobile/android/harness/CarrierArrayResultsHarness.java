import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class CarrierArrayResultsHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("owned carrier array result"); }
    static Object call(int route, Facade self, int begin, int end, Class<?> token) throws Throwable {
        if (route == 0) return Core.read(self, begin, end, token);
        if (route == 1) return CarrierArrayResultsCaller.call(self, begin, end, token);
        if (route == 3) return MethodHandles.publicLookup().findStatic(Core.class, "read",
            MethodType.methodType(String[].class, Facade.class, int.class, int.class, Class.class))
            .invoke(self, begin, end, token);
        try { return Core.class.getMethod("read", Facade.class, int.class, int.class, Class.class).invoke(null, self, begin, end, token); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Provider provider = new Provider(); RuntimeException problem = new IllegalStateException("owned provider failure");
        Object sentinel = new Object(); Object[] unseen = {sentinel};
        for (int attempt = 0; attempt < 2; attempt++) {
            Object[] reply = mode == 1 ? null : mode == 2 ? new Object[]{"a", "b"}
                : mode == 3 ? new Integer[]{1, 2} : mode == 4 ? new String[][]{{"a"}, {"b"}}
                : new String[]{"first-" + attempt, "second-" + attempt};
            Facade self = mode == 5 ? null : provider;
            Class<?> token = mode == 6 ? null : mode == 15 ? Integer.class : mode == 17 ? int.class : String.class;
            int begin = mode == 7 ? -7 : mode == 16 ? Integer.MAX_VALUE : 7;
            int end = mode == 7 ? -13 : mode == 16 ? Integer.MIN_VALUE : 13;
            int[] callbacks = {0};
            Trace.events = 0; Trace.self = sentinel; Trace.token = sentinel; Trace.result = unseen;
            Trace.begin = 99; Trace.end = 101; Trace.reply = reply; Trace.failure = problem;
            Trace.failAt = mode >= 10 && mode <= 13 ? mode - 9 : 0;
            Trace.callback = mode == 8 ? null : () -> {
                require(Trace.self == self && Trace.token == token && Trace.begin == begin && Trace.end == end && Trace.reply == reply);
                callbacks[0]++; if (mode == 9) throw problem;
                if (mode == 14) { reply[0] = "updated"; reply[1] = sentinel; }
            };
            int calls = Trace.calls; Object result = sentinel; Throwable failure = null;
            try { result = call(route, self, begin, end, token); }
            catch (Throwable error) { failure = error; }
            boolean writes = mode != 5 && mode != 10;
            boolean providerReturned = !(mode == 5 || mode >= 8 && mode <= 12 || mode == 14);
            int events = mode == 5 ? 0 : mode == 10 ? 1
                : mode == 8 || mode == 9 || mode == 11 || mode == 14 ? 12 : mode == 12 ? 123 : 1234;
            require(Trace.events == events && Trace.calls == calls + (writes ? 1 : 0));
            require(Trace.self == (writes ? self : sentinel) && Trace.token == (writes ? token : sentinel));
            require(Trace.begin == (writes ? begin : 99) && Trace.end == (writes ? end : 101));
            require(Trace.reply == reply && Trace.result == (providerReturned ? reply : unseen));
            require(callbacks[0] == (mode == 5 || mode == 8 || mode == 10 || mode == 11 ? 0 : 1));
            if (mode >= 2 && mode <= 4) require(failure instanceof ClassCastException);
            else if (mode == 5 || mode == 8) require(failure instanceof NullPointerException);
            else if (mode == 9 || mode >= 10 && mode <= 13) require(failure == problem);
            else if (mode == 14) require(failure instanceof ArrayStoreException && reply[0] == "updated" && reply[1] != sentinel);
            else require(failure == null && result == reply);
            if (providerReturned && reply != null && reply instanceof String[]) {
                reply[0] = "aliased"; require(Trace.result[0] == reply[0]);
            }
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + events);
        }
    }
}
