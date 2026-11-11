import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class NestedCarrierHarness {
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("nested carrier effects, identity or call order changed");
    }
    static Box<?> call(Fn<?,?,?> callback, long flags, int route) throws Throwable {
        if (route == 0) return Client.call(callback, flags);
        if (route == 1) return NestedCarrierCaller.call(callback, flags);
        if (route == 2) {
            try { return (Box<?>)Client.class.getMethod("call", Fn.class, long.class).invoke(null, callback, flags); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        return (Box<?>)MethodHandles.lookup().findStatic(Client.class, "call",
                MethodType.methodType(Box.class, Fn.class, long.class)).invokeExact((Fn)callback, flags);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Trace.failure = new RuntimeException("owned nested carrier failure");
        Trace.failAt = mode >= 1 && mode <= 6 ? mode : 0;
        Token token = mode == 13 ? null : new Token();
        int[] calls = {0,0,0};
        Port<Token> port = value -> { Trace.hit(4); require(value == token); calls[1]++; if(value != null) value.value++; };
        Sink<Token> sink = value -> { Trace.hit(5); require(value == token); calls[2]++; if(value != null) value.value++; };
        Port<?> wrongPort = (Port<String>)(value -> { throw new AssertionError("missing port cast"); });
        Sink<?> wrongSink = (Sink<String>)(value -> { throw new AssertionError("missing sink cast"); });
        Trace.port = mode == 8 ? null : mode == 11 ? wrongPort : port;
        Trace.sink = mode == 9 ? null : mode == 12 ? wrongSink : sink;
        Box<Token> box = new Box<Token>(); box.value = token;
        Trace.box = mode == 10 ? null : box;
        Object callbackResult = new Object(), untouched = new Object();
        Fn<Port<? super Token>,Sink<? super Token>,Object> callback = (p,s) -> {
            calls[0]++; Trace.hit(3);
            require(p == Trace.port && s == Trace.sink);
            if(p != null) p.send(token);
            if(s != null) s.accept(token);
            Trace.hit(6); return callbackResult;
        };
        for(int attempt = 0; attempt < 2; attempt++) {
            long flags = attempt == 0 ? Long.MIN_VALUE : Long.MAX_VALUE;
            Trace.events = 0; Trace.callbackResult = untouched;
            int oldCallbacks=calls[0], oldPorts=calls[1], oldSinks=calls[2];
            Throwable failure = null; Box<?> result = null;
            try { result = call(mode == 7 ? null : callback, flags, route); }
            catch(Throwable error) { failure = error; }
            String expected = mode == 1 ? "" : mode == 2 || mode == 7 ? "2" :
                    mode == 3 || mode == 11 ? "23" : mode == 4 || mode == 12 ? "234" :
                    mode == 5 ? "2345" : mode == 8 ? "2356" : mode == 9 ? "2346" : "23456";
            if(attempt == 0) expected = "1"+expected;
            require(Trace.events == (expected.isEmpty() ? 0 : Long.parseLong(expected)));
            if(mode == 1) {
                if(attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if(mode >= 2 && mode <= 6) require(failure == Trace.failure);
            else if(mode == 7) require(failure instanceof NullPointerException);
            else if(mode == 11 || mode == 12) require(failure instanceof ClassCastException);
            else require(failure == null && result == Trace.box && (result == null || result.value == token));
            boolean completed = mode == 0 || mode == 8 || mode == 9 || mode == 10 || mode == 13;
            require(Trace.callbackResult == (completed ? callbackResult : untouched));
            require(Trace.flags == (mode == 1 ? 0 : flags));
            require(calls[0] == oldCallbacks + (mode != 1 && mode != 2 && mode != 7 ? 1 : 0));
            require(calls[1] == oldPorts + (mode != 1 && mode != 2 && mode != 3 && mode != 4 && mode != 7 && mode != 8 && mode != 11 ? 1 : 0));
            require(calls[2] == oldSinks + (mode == 0 || mode == 6 || mode == 8 || mode == 10 || mode == 13 ? 1 : 0));
            if(token != null) require(token.value == calls[1] + calls[2]);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
