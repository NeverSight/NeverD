import fixture.*;
import java.lang.reflect.*;

public final class RecursiveEnumDefaultHarness {
    private enum Choice { A, B }
    private enum Other { VALUE }
    private interface Call {
        Enum<?> run(Reader<Object> receiver, Class<?> type, Object key, Enum<?> value) throws Throwable;
    }
    private static int identities, states, failures;

    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("reference identity changed");
        identities++;
    }

    private static void state(Reader<?> receiver, int before, Class<?> type, Object key) {
        if (receiver.calls != before + 1) throw new AssertionError("default effects changed");
        identity(receiver.type, type);
        identity(receiver.seen, key);
        states++;
    }

    private static void bound(Method method) {
        TypeVariable<Method> variable = method.getTypeParameters()[0];
        ParameterizedType upper = (ParameterizedType) variable.getBounds()[0];
        if (upper.getRawType() != Enum.class || upper.getActualTypeArguments()[0] != variable
                || method.getGenericReturnType() != variable)
            throw new AssertionError("complete recursive Enum bound changed");
        Type[] params = method.getGenericParameterTypes();
        if (((ParameterizedType) params[0]).getRawType() != Class.class
                || ((ParameterizedType) params[0]).getActualTypeArguments()[0] != variable
                || params[1] != Api.class.getTypeParameters()[0] || params[2] != variable
                || method.getExceptionTypes().length != 0 || !method.isDefault())
            throw new AssertionError("class and method scopes or default ownership changed");
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Throwable {
        Method choose = Api.class.getMethod("choose", Class.class, Object.class, Enum.class);
        bound(choose);
        if (Refined.class.getDeclaredMethods().length != 0
                || Reader.class.getMethod("choose", Class.class, Object.class, Enum.class).getDeclaringClass() != Api.class)
            throw new AssertionError("default implementation was copied or shadowed");
        ParameterizedType parent = (ParameterizedType) Reader.class.getGenericInterfaces()[0];
        if (parent.getRawType() != Refined.class || parent.getActualTypeArguments()[0] != Reader.class.getTypeParameters()[0])
            throw new AssertionError("receiver class scope changed");
        for (Method method : Reader.class.getDeclaredMethods())
            if (method.isBridge()) throw new AssertionError("unnecessary bridge introduced");
        Call[] calls = {
            (r, t, k, v) -> r.choose((Class) t, k, (Enum) v),
            (r, t, k, v) -> ((Refined) r).choose((Class) t, k, (Enum) v),
            (r, t, k, v) -> ((Api) r).choose((Class) t, k, (Enum) v),
            (r, t, k, v) -> ((Api<Object>) r).choose((Class) t, k, (Enum) v),
            (r, t, k, v) -> {
                try { return (Enum<?>) choose.invoke(r, t, k, v); }
                catch (InvocationTargetException problem) { throw problem.getCause(); }
            }
        };
        Reader<Object> reader = new Reader<>();
        Object[] keys = {null, "owned", new Object()};
        Class<?>[] types = {Choice.class, null};
        Enum<?>[] values = {Choice.A, Choice.B, Other.VALUE, null};
        for (Object key : keys) for (Class<?> type : types) for (Enum<?> value : values)
            for (Call call : calls) {
                int before = reader.calls;
                identity(call.run(reader, type, key, value), value);
                state(reader, before, type, key);
            }
        RuntimeException problem = new IllegalStateException("owned recursive default failure");
        reader.failure = problem;
        for (Call call : calls) {
            int before = reader.calls;
            try { call.run(reader, Choice.class, keys[2], Choice.A); throw new AssertionError("failure missing"); }
            catch (RuntimeException actual) { identity(actual, problem); failures++; }
            state(reader, before, Choice.class, keys[2]);
        }
        reader.failure = null;
        for (Call call : calls) {
            int before = reader.calls;
            try { call.run(null, null, null, null); throw new AssertionError("null receiver accepted"); }
            catch (NullPointerException expected) { failures++; }
            if (reader.calls != before) throw new AssertionError("null receiver caused default effects");
        }
        System.out.println("values=24,identities=" + identities + ",states=" + states + ",failures=" + failures + ",bridges=0");
        System.out.println("recursive-enum-scopes=retained,original-default-ownership-and-effects=matched");
    }
}
