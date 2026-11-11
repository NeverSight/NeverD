import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class MappedCarrierHarness {
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("mapped carrier effects, identity or call order changed");
    }
    static Box<?> call(Box<?> seed, Fn<?,?> callback, long flags, int route) throws Throwable {
        if (route == 0) return Client.call(seed, callback, flags);
        if (route == 1) return MappedCarrierCaller.call(seed, callback, flags);
        if (route == 2) {
            try { return (Box<?>)Client.class.getMethod("call", Box.class, Fn.class, long.class).invoke(null, seed, callback, flags); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        return (Box<?>)MethodHandles.lookup().findStatic(Client.class, "call",
                MethodType.methodType(Box.class, Box.class, Fn.class, long.class)).invokeExact((Box)seed, (Fn)callback, flags);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Trace.failure = new RuntimeException("owned mapped carrier failure");
        Trace.failAt = mode >= 1 && mode <= 4 ? mode : 0;
        int[] value = new int[]{0}, other = new int[]{31}, calls = {0};
        Object seed = mode == 6 ? "incompatible seed" : mode == 7 ? null : value;
        StringBuilder callbackResult = mode == 8 ? null : new StringBuilder("result");
        Box<Object> input = new Box<Object>();
        Object untouched = new Object();
        Error callbackFailure = new AssertionError("owned callback error");
        Box<Object> box = new Box<Object>(); box.value = callbackResult;
        Trace.box = mode == 9 ? null : box;
        Fn<int[],StringBuilder> callback = incoming -> {
            calls[0]++; Trace.hit(3); require(incoming == seed);
            if (mode == 11) throw callbackFailure;
            if (incoming != null) incoming[0]++;
            if (mode == 10) input.value = other;
            return callbackResult;
        };
        for(int attempt = 0; attempt < 2; attempt++) {
            long flags = attempt == 0 ? Long.MIN_VALUE : Long.MAX_VALUE;
            Trace.events = 0; Trace.callbackResult = untouched; input.value = seed;
            int oldCallbacks = calls[0], oldValue = value[0];
            Throwable failure = null; Box<?> result = null;
            try { result = call(mode == 12 ? null : input, mode == 5 ? null : callback, flags, route); }
            catch(Throwable error) { failure = error; }
            String expected = mode == 1 ? "" : mode == 2 || mode == 5 || mode == 6 || mode == 12 ? "2" :
                    mode == 3 || mode == 11 ? "23" : "234";
            if(attempt == 0) expected = "1"+expected;
            require(Trace.events == (expected.isEmpty() ? 0 : Long.parseLong(expected)));
            if(mode == 1) {
                if(attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if(mode >= 2 && mode <= 4) require(failure == Trace.failure);
            else if(mode == 5 || mode == 12) require(failure instanceof NullPointerException);
            else if(mode == 6) require(failure instanceof ClassCastException);
            else if(mode == 11) require(failure == callbackFailure);
            else require(failure == null && result == Trace.box && (result == null || result.value == callbackResult));
            boolean completed = mode == 0 || mode == 4 || mode >= 7 && mode <= 10;
            require(Trace.callbackResult == (completed ? callbackResult : untouched));
            require(Trace.flags == (mode == 1 ? 0 : flags));
            require(calls[0] == oldCallbacks + (mode != 1 && mode != 2 && mode != 5 && mode != 6 && mode != 12 ? 1 : 0));
            require(value[0] == oldValue + (completed && mode != 7 ? 1 : 0));
            require(input.value == (mode == 10 ? other : seed));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
