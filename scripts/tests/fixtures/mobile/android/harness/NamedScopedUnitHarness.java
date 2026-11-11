import fixture.*;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.TypeVariable;

public final class NamedScopedUnitHarness {
    private static int values, identities, states, failures;
    private static Method erased, typed;
    private static MethodHandle erasedHandle, typedHandle;
    private static void state(int actual, int expected) {
        if (actual != expected) throw new AssertionError("state: " + actual + ":" + expected); states++;
    }
    private static void value(int actual, int expected) {
        if (actual != expected) throw new AssertionError("value"); values++;
    }
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("identity"); identities++;
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static Object invoke(int route, Core receiver, Object first, Object second) throws Throwable {
        try {
            switch (route) {
                case 0: return ((BiTask) receiver).apply(first, second);
                case 1: return erased.invoke(receiver, first, second);
                case 2: return erasedHandle.invoke(receiver, first, second);
                case 3: return ((BiTask<Object, Throwable, kotlin.Unit>) receiver).apply(first, (Throwable) second);
                case 4: receiver.forward(first, (Throwable) second); return null;
                case 5: return typed.invoke(receiver, first, second);
                case 6: typedHandle.invoke(receiver, first, second); return null;
                default: throw new AssertionError("route");
            }
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        String sourceName = args[0];
        boolean rebuilt = !sourceName.equals("apply");
        int wrappers = 0;
        for (Method method : Core.class.getDeclaredMethods()) {
            if (method.getName().equals("apply") && method.getReturnType() == Object.class) erased = method;
            if (method.getName().equals(sourceName) && method.getReturnType() == void.class) typed = method;
            if (method.getName().equals("apply") && method.getReturnType() == kotlin.Unit.class) wrappers++;
        }
        if (erased == null || typed == null || !erased.isBridge() || !erased.isSynthetic()
                || wrappers != (rebuilt ? 1 : 0)
                || !(typed.getGenericParameterTypes()[0] instanceof TypeVariable)
                || ((TypeVariable<?>) typed.getGenericParameterTypes()[0]).getGenericDeclaration() != Core.class
                || !Core.class.getGenericSuperclass().getTypeName().equals("fixture.Parent<E>")
                || !Core.class.getGenericInterfaces()[0].getTypeName().equals("fixture.BiTask<E, java.lang.Throwable, kotlin.Unit>"))
            throw new AssertionError("complete original scope or reported method mapping");
        erasedHandle = MethodHandles.publicLookup().unreflect(erased);
        typedHandle = MethodHandles.publicLookup().unreflect(typed);
        Core<Object> receiver = new Core<Object>();
        if (args.length > 1) {
            int route = Integer.parseInt(args[1]);
            RuntimeException problem = new IllegalStateException("owned-initialization");
            Trace.initializationFailure = problem;
            Object first = new Object(); Throwable second = new Throwable("owned-input");
            receiver.forward(first, second); state(receiver.calls, 1); state(Trace.initializations, 0);
            try { invoke(route, receiver, first, second); throw new AssertionError("initialization failure missing"); }
            catch (ExceptionInInitializerError actual) { identity(actual.getCause(), problem); failures++; }
            state(receiver.calls, 2); state(Trace.initializations, 1);
            identity(receiver.value, first); identity(receiver.error, second);
            try { invoke(route, receiver, first, second); throw new AssertionError("erroneous class accepted"); }
            catch (NoClassDefFoundError expected) { failures++; }
            state(receiver.calls, 3); state(Trace.initializations, 1);
            identity(receiver.value, first); identity(receiver.error, second);
            receiver.forward(first, second); state(receiver.calls, 4); state(Trace.initializations, 1);
        } else {
            state(Trace.initializations, 0);
            RuntimeException problem = new IllegalStateException("owned-call-failure");
            receiver.failure = problem;
            for (int route : new int[]{4, 0}) {
                try { invoke(route, receiver, null, null); throw new AssertionError("failure missing"); }
                catch (RuntimeException actual) { identity(actual, problem); failures++; }
                state(receiver.calls, route == 4 ? 1 : 2); state(Trace.initializations, 0);
            }
            receiver.failure = null;
            receiver.forward(null, null); state(receiver.calls, 3); state(Trace.initializations, 0);
            Object unit = invoke(0, receiver, null, null);
            identity(unit, kotlin.Unit.INSTANCE); state(receiver.calls, 4); state(Trace.initializations, 1);
            Object[] inputs = {null, new Object(), new int[]{7}, "owned"};
            Throwable[] errors = {null, new IllegalArgumentException("owned-argument")};
            for (int route = 0; route < 7; route++) for (Object input : inputs)
                for (Throwable error : errors) for (boolean throwing : new boolean[]{false, true}) {
                    receiver.failure = throwing ? problem : null;
                    int before = receiver.calls;
                    try {
                        Object result = invoke(route, receiver, input, error);
                        if (throwing) throw new AssertionError("failure missing");
                        identity(result, route < 4 ? unit : null);
                        if (input instanceof int[]) {
                            int old = ((int[]) input)[0]; ((int[]) receiver.value)[0]++;
                            value(((int[]) input)[0], old + 1);
                        }
                    } catch (RuntimeException actual) {
                        if (!throwing) throw actual;
                        identity(actual, problem); failures++;
                    }
                    identity(receiver.value, input); identity(receiver.error, error);
                    state(receiver.calls, before + 1); state(Trace.initializations, 1);
                }
            receiver.failure = problem;
            for (int route = 0; route < 4; route++)
                for (Object invalid : new Object[]{new Object(), "wrong", new int[0]}) {
                    int before = receiver.calls; Object old = receiver.value; Throwable oldError = receiver.error;
                    try { invoke(route, receiver, null, invalid); throw new AssertionError("bad cast accepted"); }
                    catch (ClassCastException expected) { failures++; }
                    state(receiver.calls, before); state(Trace.initializations, 1);
                    identity(receiver.value, old); identity(receiver.error, oldError);
                }
            for (int route = 0; route < 7; route++) {
                try { invoke(route, null, null, null); throw new AssertionError("null receiver accepted"); }
                catch (NullPointerException expected) { failures++; }
            }
            receiver.__neverdVoidBridgeTarget(); state(receiver.collisions, 1);
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("reflection=" + (rebuilt ? "reported-source-mapping" : "original"));
    }
}
