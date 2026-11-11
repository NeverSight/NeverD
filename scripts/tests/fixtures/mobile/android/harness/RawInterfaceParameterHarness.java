import fixture.Api;
import fixture.Core;
import fixture.Payload;
import fixture.Sink;
import fixture.Token;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
public final class RawInterfaceParameterHarness {
    static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    static Object call(Core<?> core, Object value, Sink sink, int route) throws Throwable {
        if (route == 0) return ((Api)core).emit(value, sink);
        if (route == 1) return RawInterfaceParameterCaller.call(core, value, sink);
        if (route == 2) {
            try { return Core.class.getMethod("emit", Object.class, Sink.class).invoke(core, value, sink); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (route == 3) return MethodHandles.publicLookup().findVirtual(Api.class, "emit",
                MethodType.methodType(Object.class, Object.class, Sink.class)).invoke(core, value, sink);
        return core.emit((Payload)value, sink);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        boolean original = Boolean.parseBoolean(args[2]);
        boolean raw = Boolean.parseBoolean(args[3]);
        require((Core.class.getGenericInterfaces()[0] instanceof Class) == raw, "original raw interface metadata changed");
        require(Core.class.getTypeParameters().length == 1, "original owner formal changed");
        Method erased = Core.class.getMethod("emit", Object.class, Sink.class);
        require(erased.isBridge() == original && erased.isSynthetic() == original, "unreported bridge flags");
        Method typed = Core.class.getMethod("emit", Payload.class, Sink.class);
        require(typed.getGenericParameterTypes()[1].getTypeName().equals("fixture.Sink<? super fixture.Token>"),
                "closed generic tail changed");
        Trace.failure = new RuntimeException("owned bridge failure");
        Trace.failAt = mode == 4 ? 1 : mode == 5 ? 2 : mode == 6 ? 3 : 0;
        Payload payload = new Payload();
        Token token = new Token();
        Trace.token = mode == 8 ? null : token;
        Trace.result = mode == 7 ? null : new Object();
        Object input = mode == 1 ? null : mode == 2 ? "wrong incoming type" : payload;
        Core<?> core = mode == 10 ? null : new Core<Object>();
        int[] calls = {0};
        Sink<Token> callback = value -> {
            Trace.step(2);
            require(value == Trace.token && Trace.seen == input, "tail identity or prior effect changed");
            calls[0]++;
            if (value != null) value.value++;
        };
        Sink<?> wrong = (Sink<String>)(value -> { throw new AssertionError("wrong tail bridge did not cast"); });
        Sink sink = mode == 3 ? null : mode == 9 ? wrong : callback;
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.trace = 0;
            int oldCalls = calls[0];
            Object result = null;
            Throwable error = null;
            try { result = call(core, input, sink, route); }
            catch (Throwable failure) { error = failure; }
            int expected = mode == 2 || mode == 10 ? 0 : mode == 4 || mode == 9 ? 1 :
                    mode == 5 ? 12 : mode == 3 ? 13 : 123;
            require(Trace.trace == expected, "bridge cast, call or failure order changed");
            if (mode == 2 || mode == 9) require(error instanceof ClassCastException, "original cast failure changed");
            else if (mode == 10) require(error instanceof NullPointerException, "null receiver failure changed");
            else if (mode >= 4 && mode <= 6) require(error == Trace.failure, "original exception identity changed");
            else {
                require(error == null && result == Trace.result, "original return identity changed");
                require(Trace.seen == input, "incoming reference changed");
                payload.value++;
            }
            boolean callbackCompleted = mode != 2 && mode != 3 && mode != 4 && mode != 5 && mode != 9 && mode != 10;
            require(calls[0] == oldCalls + (callbackCompleted ? 1 : 0), "tail invocation count changed");
            if (mode != 8) require(token.value == calls[0], "mutable token alias changed");
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.trace);
        }
    }
}
