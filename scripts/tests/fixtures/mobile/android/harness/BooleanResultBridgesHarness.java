import fixture.*;
import java.lang.reflect.*;

public final class BooleanResultBridgesHarness {
    private static int values, identities, states, failures;

    private interface Call { Object run() throws Throwable; }

    private static Object reflect(Method method, Object receiver) throws Throwable {
        try { return method.invoke(receiver); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }

    private static Method entry(String name, Class<?> result) {
        Method found = null;
        for (Method method : Receiver.class.getDeclaredMethods()) {
            if (method.getName().equals(name) && method.getReturnType() == result
                    && method.getParameterCount() == 0) {
                if (found != null) throw new AssertionError("duplicate original return entry");
                found = method;
            }
        }
        if (found == null) throw new AssertionError("missing original return entry");
        return found;
    }

    private static void state(Receiver<?, ?> receiver, int calls, boolean value) {
        if (receiver.calls != calls || receiver.value != value)
            throw new AssertionError("target call count or mutation order changed");
        states++;
    }

    private static void result(Object actual, boolean expected, boolean cached) {
        if (!(actual instanceof Boolean) || ((Boolean) actual).booleanValue() != expected)
            throw new AssertionError("boolean result changed");
        values++;
        if (cached) {
            if (actual != (expected ? Boolean.TRUE : Boolean.FALSE))
                throw new AssertionError("original Boolean.valueOf identity changed");
            identities++;
        }
    }

    private static void failure(Call call, Class<? extends Throwable> kind,
                                RuntimeException same) {
        try {
            call.run();
            throw new AssertionError("failure missing");
        } catch (Throwable error) {
            if (error.getClass() != kind || (same != null && error != same))
                throw new AssertionError("exception type or identity changed", error);
            if (same != null) identities++;
            failures++;
        }
    }

    public static void main(String[] args) throws Throwable {
        boolean original = args[0].equals("original");
        Method primitive = entry(args[1], boolean.class);
        Method bridge = entry("invoke", Object.class);
        if (!Modifier.isPublic(primitive.getModifiers()) || !Modifier.isFinal(primitive.getModifiers())
                || primitive.isBridge() || primitive.isSynthetic()
                || bridge.isBridge() != original || bridge.isSynthetic() != original)
            throw new AssertionError("reported original entry flags changed");
        Type contract = Receiver.class.getGenericInterfaces()[0];
        if (original) {
            if (!(contract instanceof ParameterizedType)
                    || ((ParameterizedType) contract).getRawType() != Function.class
                    || ((ParameterizedType) contract).getActualTypeArguments()[0] != Boolean.class)
                throw new AssertionError("original Boolean result contract changed");
        } else if (contract != Function.class) {
            throw new AssertionError("reported raw interface projection missing");
        }
        if (Receiver.class.getTypeParameters().length != 2
                || !Receiver.class.getTypeParameters()[0].getName().equals("S")
                || !Receiver.class.getTypeParameters()[1].getName().equals("V"))
            throw new AssertionError("unrelated original generic scopes changed");

        Receiver<Object, Object> receiver = new Receiver<>();
        Call[] calls = {
            () -> Caller.primitive(receiver),
            () -> Caller.direct(receiver),
            () -> Caller.erased(receiver),
            () -> reflect(primitive, receiver),
            () -> reflect(bridge, receiver)
        };
        int count = 0;
        boolean value = false;
        for (int round = 0; round < 16; round++) {
            receiver.value = value = (round & 1) != 0;
            for (int path = 0; path < calls.length; path++) {
                value = !value;
                result(calls[path].run(), value, path == 1 || path == 2 || path == 4);
                state(receiver, ++count, value);
            }
        }
        RuntimeException problem = new IllegalStateException("owned Boolean callback failure");
        receiver.failure = problem;
        for (Call call : calls) {
            failure(call, IllegalStateException.class, problem);
            state(receiver, ++count, value = !value);
        }
        receiver.failure = null;
        Call[] absent = {
            () -> Caller.primitive(null),
            () -> Caller.direct(null),
            () -> Caller.erased(null),
            () -> reflect(primitive, null),
            () -> reflect(bridge, null)
        };
        for (Call call : absent) {
            failure(call, NullPointerException.class, null);
            state(receiver, count, value);
        }
        if (receiver.__neverdReturnSlot() != 37)
            throw new AssertionError("source alias collided with an original name");
        values++;
        System.out.println("values=" + values + ",identities=" + identities
                + ",states=" + states + ",failures=" + failures);
        System.out.println(original ? "reflection=original"
                : "reflection=reported-raw-interface-and-primitive-name");
    }
}
