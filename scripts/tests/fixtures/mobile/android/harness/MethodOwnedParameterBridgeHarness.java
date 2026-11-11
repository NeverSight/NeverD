import fixture.*;
import java.lang.reflect.*;
import java.lang.annotation.RetentionPolicy;
import java.util.*;

public final class MethodOwnedParameterBridgeHarness {
    private static int values, identities, states, failures;
    private interface Call { Object run(Class<?> kind, Object key) throws Throwable; }
    private static final class Leaf extends Core {
        public <N extends Enum<N>> N pick(Class<N> kind, String key) {
            Trace.calls += 2;
            return super.pick(kind, key);
        }
    }
    private static Object invoke(Method method, Object receiver, Class<?> kind, Object key) throws Throwable {
        try { return method.invoke(receiver, kind, key); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("original generic bridge identity changed");
        identities++;
    }
    private static void run(Call call, Class<?> kind, Object key, Object value,
                            int delta, Class<?> expectedFailure, RuntimeException problem) {
        Trace.calls = 0; Trace.kind = null; Trace.key = null;
        Trace.value = value; Trace.failure = problem;
        try {
            Object result = call.run(kind, key);
            if (expectedFailure != null) throw new AssertionError("original bridge failure missing");
            same(result, value); values++;
        } catch (Throwable error) {
            if (expectedFailure == null || error.getClass() != expectedFailure)
                throw new AssertionError("original bridge failure type or order changed", error);
            if (problem != null) same(error, problem);
            failures++;
        }
        if (Trace.calls != delta) throw new AssertionError("original bridge virtual dispatch changed");
        same(Trace.kind, delta == 0 ? null : kind);
        same(Trace.key, delta == 0 ? null : key);
        same(Trace.value, value); states++;
    }
    private static void contract(Method method, String name) {
        TypeVariable<Method>[] formals = method.getTypeParameters();
        if (formals.length != 1 || !formals[0].getName().equals(name)
                || !formals[0].getGenericDeclaration().equals(method)
                || method.getReturnType() != Enum.class || !method.getGenericReturnType().equals(formals[0])
                || method.getExceptionTypes().length != 0 || method.isBridge())
            throw new AssertionError("method-owned original scope changed");
        ParameterizedType bound = (ParameterizedType) formals[0].getBounds()[0];
        ParameterizedType kind = (ParameterizedType) method.getGenericParameterTypes()[0];
        if (bound.getRawType() != Enum.class || kind.getRawType() != Class.class
                || !Arrays.equals(bound.getActualTypeArguments(), new Type[] {formals[0]})
                || !Arrays.equals(kind.getActualTypeArguments(), new Type[] {formals[0]}))
            throw new AssertionError("recursive self bound or invariant Class argument changed");
    }
    @SuppressWarnings({"unchecked", "rawtypes"})
    public static void main(String[] args) throws Exception {
        Method typed = Core.class.getDeclaredMethod("pick", Class.class, String.class);
        Method bridge = Core.class.getDeclaredMethod("pick", Class.class, Object.class);
        Method declaration = Contract.class.getDeclaredMethod("pick", Class.class, Object.class);
        contract(typed, "N"); contract(declaration, "M");
        if (!bridge.isBridge() || !bridge.isSynthetic() || bridge.getTypeParameters().length != 0
                || bridge.getReturnType() != Enum.class || bridge.getExceptionTypes().length != 0
                || typed.getGenericParameterTypes()[1] != String.class
                || !declaration.getGenericParameterTypes()[1].equals(Contract.class.getTypeParameters()[0]))
            throw new AssertionError("original erased bridge metadata or class scope changed");
        for (Core receiver : new Core[] {new Core(), new Leaf()}) {
            int delta = receiver instanceof Leaf ? 3 : 1;
            Contract view = receiver;
            Call[] paths = {
                (kind, key) -> receiver.pick((Class) kind, (String) key),
                (kind, key) -> view.pick((Class) kind, key),
                (kind, key) -> invoke(typed, receiver, kind, key),
                (kind, key) -> invoke(bridge, receiver, kind, key),
                (kind, key) -> invoke(declaration, receiver, kind, key)
            };
            for (Call path : paths) {
                for (Object[] test : new Object[][] {
                        {Thread.State.class, "first", Thread.State.NEW},
                        {Thread.State.class, null, Thread.State.RUNNABLE},
                        {RetentionPolicy.class, "second", RetentionPolicy.RUNTIME},
                        {RetentionPolicy.class, "absent", null}})
                    run(path, (Class<?>) test[0], test[1], test[2], delta, null, null);
                run(path, null, "missing kind", Thread.State.NEW, delta, NullPointerException.class, null);
                run(path, Thread.State.class, "wrong enum", RetentionPolicy.SOURCE, delta, ClassCastException.class, null);
                RuntimeException problem = new IllegalStateException("owned method failure");
                run(path, Thread.State.class, "failure", Thread.State.NEW, delta, problem.getClass(), problem);
            }
            for (int route : new int[] {1, 3, 4})
                run(paths[route], Thread.State.class, Integer.valueOf(7), Thread.State.NEW, 0, ClassCastException.class, null);
        }
        Call[] absent = {
            (kind, key) -> ((Core) null).pick((Class) kind, (String) key),
            (kind, key) -> ((Contract) null).pick((Class) kind, key),
            (kind, key) -> invoke(typed, null, kind, key),
            (kind, key) -> invoke(bridge, null, kind, key),
            (kind, key) -> invoke(declaration, null, kind, key)
        };
        for (Call path : absent)
            run(path, Thread.State.class, "null receiver", Thread.State.NEW, 0, NullPointerException.class, null);
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("recursive-method-bounds=retained,invariant-class-arguments=retained,original-checked-bridge=retained");
    }
}
