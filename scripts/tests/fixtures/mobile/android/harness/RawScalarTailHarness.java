import fixture.Api;
import fixture.Core;
import fixture.Scalar;
import fixture.Sink;
import fixture.Token;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class RawScalarTailHarness {
    static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    static Object call(Core<?> core, Object value, Sink sink, int route) throws Throwable {
        if (route == 0) return ((Api)core).emit(value, sink);
        if (route == 1) return RawScalarTailCaller.call(core, value, sink);
        if (route == 2) {
            try { return Core.class.getMethod("emit", Object.class, Sink.class).invoke(core, value, sink); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (route == 3) return MethodHandles.publicLookup().findVirtual(Api.class, "emit",
                MethodType.methodType(Object.class, Object.class, Sink.class)).invoke(core, value, sink);
        return core.emit(value, sink);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        boolean bridge = Boolean.parseBoolean(args[2]);
        require(Core.class.getGenericInterfaces()[0] instanceof Class, "raw owner edge changed");
        require(Core.class.getTypeParameters().length == 1, "original owner formal changed");
        Method erased = Core.class.getMethod("emit", Object.class, Sink.class);
        require(erased.isBridge() == bridge && erased.isSynthetic() == bridge, "unreported bridge flags");
        Method typed = Core.class.getMethod("emitTyped", long.class, Sink.class);
        require(typed.getGenericParameterTypes()[1].getTypeName().equals("fixture.Sink<? super fixture.Token>"),
                "closed generic tail changed");
        Trace.failure = new RuntimeException("owned getter or callback failure");
        Error fatal = new Error("owned callback error");
        Trace.failAt = mode >= 4 && mode <= 7 ? mode - 3 : 0;
        Scalar scalar = new Scalar();
        Token token = new Token();
        Trace.token = mode == 9 ? null : token;
        Trace.result = mode == 8 ? null : new Object();
        Object replacement = new Object();
        Object input = mode == 1 ? null : mode == 2 ? "wrong incoming type" : scalar;
        Core<?> core = mode == 11 ? null : new Core<Object>();
        int[] calls = {0};
        long[] expectedValue = {0};
        Sink<Token> callback = value -> {
            Trace.step(3);
            require(value == Trace.token && Trace.seen == scalar && Trace.accepted == expectedValue[0],
                    "scalar conversion or tail origin changed");
            calls[0]++;
            if (value != null) value.value++;
            if (mode == 12) Trace.result = replacement;
            if (mode == 15) throw fatal;
        };
        Sink<?> wrong = (Sink<String>)(value -> { throw new AssertionError("wrong tail bridge did not cast"); });
        Sink sink = mode == 3 ? null : mode == 10 ? wrong : callback;
        Trace.bits = mode == 13 ? 0x7fedcba987654321L : mode == 14 ? Long.MIN_VALUE : -1L;
        Trace.seen = replacement;
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.trace = 0;
            Trace.accepted = 41;
            long before = Trace.bits;
            expectedValue[0] = before;
            Object seenBefore = Trace.seen;
            int oldCalls = calls[0];
            Object result = null;
            Throwable error = null;
            try { result = call(core, input, sink, route); }
            catch (Throwable failure) { error = failure; }
            int expected = mode == 1 || mode == 2 || mode == 11 ? 0 : mode == 4 ? 1 :
                    mode == 5 || mode == 10 ? 12 : mode == 6 || mode == 15 ? 123 :
                    mode == 3 ? 124 : 1234;
            require(Trace.trace == expected, "scalar bridge cast, call or failure order changed");
            if (mode == 2 || mode == 10) require(error instanceof ClassCastException, "original cast failure changed");
            else if (mode == 1 || mode == 11) require(error instanceof NullPointerException, "null receiver failure changed");
            else if (mode >= 4 && mode <= 7) require(error == Trace.failure, "original exception identity changed");
            else if (mode == 15) require(error == fatal, "original Error identity changed");
            else require(error == null && result == Trace.result, "original return identity changed");
            boolean decoded = mode != 1 && mode != 2 && mode != 4 && mode != 11;
            require(Trace.bits == (decoded ? before + 7 : before), "unboxing effect count changed");
            require(Trace.seen == (decoded ? scalar : seenBefore), "unboxing receiver identity changed");
            boolean accepted = decoded && mode != 5;
            require(Trace.accepted == (accepted ? before : 41), "exact scalar words changed");
            boolean callbackCompleted = accepted && mode != 3 && mode != 6 && mode != 10;
            require(calls[0] == oldCalls + (callbackCompleted ? 1 : 0), "tail invocation count changed");
            if (mode != 9) require(token.value == calls[0], "mutable token alias changed");
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.trace);
        }
    }
}
