import fixture.*;
import java.lang.reflect.*;
import java.util.*;

public final class SourceDefaultParameterBridgesHarness {
    private static int values, identities, states, failures;
    private interface Call { int run() throws Throwable; }
    private static final class Offset extends Receiver {
        int offset;
        @Override public int order() { return super.order() + offset; }
    }
    private static int reflect(Method method, Object receiver, Object argument) throws Throwable {
        try { return ((Integer) method.invoke(receiver, argument)).intValue(); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("default argument or exception identity changed");
        identities++;
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static Call[] paths(Receiver receiver, Ordered argument, Method typed, Method bridge, Method root) {
        Ordered ordered = receiver;
        Ordering<Ordered> parent = receiver;
        Ordering raw = receiver;
        return new Call[] {
            () -> receiver.compare(argument), () -> ordered.compare(argument), () -> parent.compare(argument),
            () -> raw.compare(argument), () -> reflect(typed, receiver, argument),
            () -> reflect(bridge, receiver, argument), () -> reflect(root, receiver, argument)
        };
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static Call[] erased(Receiver receiver, Object argument, Method bridge, Method root) {
        Ordering raw = receiver;
        return new Call[] {() -> raw.compare(argument), () -> reflect(bridge, receiver, argument),
                           () -> reflect(root, receiver, argument)};
    }
    private static void execute(Call call, Receiver left, Receiver right, Ordered receiver, Ordered argument,
                                int result, int events, int defaultCalls, int leftCalls, int rightCalls,
                                Class<?> failure, RuntimeException problem) {
        Trace.calls = 0;
        Trace.events = 0;
        Trace.receiver = null;
        Trace.argument = null;
        left.calls = 0;
        right.calls = 0;
        try {
            int actual = call.run();
            if (failure != null) throw new AssertionError("default call failure missing");
            if (actual != result) throw new AssertionError("default comparison result changed");
            values++;
        } catch (Throwable error) {
            if (failure == null || error.getClass() != failure)
                throw new AssertionError("default exception type changed", error);
            if (problem != null) same(error, problem);
            failures++;
        }
        if (Trace.calls != defaultCalls || Trace.events != events
                || left.calls != leftCalls || right.calls != rightCalls)
            throw new AssertionError("default bridge cast or virtual order effects changed");
        same(Trace.receiver, receiver);
        same(Trace.argument, argument);
        states++;
    }
    private static void metadata(Method typed, Method bridge, Method root) throws Exception {
        TypeVariable<?> formal = Ordering.class.getTypeParameters()[0];
        Type parent = Ordered.class.getGenericInterfaces()[0];
        if (!(parent instanceof ParameterizedType)
                || ((ParameterizedType) parent).getRawType() != Ordering.class
                || !Arrays.equals(((ParameterizedType) parent).getActualTypeArguments(), new Type[] {Ordered.class})
                || !Arrays.equals(formal.getBounds(), new Type[] {Object.class})
                || !root.getGenericParameterTypes()[0].equals(formal)
                || !Arrays.equals(Receiver.class.getGenericInterfaces(), new Type[] {Facade.class})
                || !Arrays.equals(Facade.class.getGenericInterfaces(), new Type[] {Ordered.class})
                || Receiver.class.getTypeParameters().length != 0 || Facade.class.getTypeParameters().length != 0
                || Facade.class.getDeclaredMethods().length != 0)
            throw new AssertionError("original complete root specialization changed");
        if (!typed.isDefault() || typed.isBridge() || typed.isSynthetic()
                || !bridge.isDefault() || !bridge.isBridge() || !bridge.isSynthetic()
                || root.isDefault() || root.isBridge() || root.isSynthetic()
                || typed.getModifiers() != Modifier.PUBLIC || bridge.getModifiers() != (Modifier.PUBLIC | 0x1040)
                || root.getModifiers() != (Modifier.PUBLIC | Modifier.ABSTRACT))
            throw new AssertionError("original default owner or bridge flags changed");
        for (Method method : new Method[] {typed, bridge, root}) {
            if (method.getReturnType() != int.class || method.getTypeParameters().length != 0
                    || method.getExceptionTypes().length != 0 || method.getGenericExceptionTypes().length != 0)
                throw new AssertionError("default comparison signature or Exceptions changed");
        }
        // Select every original entry exactly; the runner separately inventories reported helpers.
        for (Method method : new Method[] {typed, bridge, root,
                Ordered.class.getDeclaredMethod("order"), Receiver.class.getDeclaredMethod("order")}) {
            if (method.getExceptionTypes().length != 0 || method.getGenericExceptionTypes().length != 0)
                throw new AssertionError("original method acquired declared Exceptions: " + method);
        }
        for (Constructor<?> constructor : new Constructor<?>[] {
                Receiver.class.getDeclaredConstructor(), Trace.class.getDeclaredConstructor()}) {
            if (constructor.getExceptionTypes().length != 0 || constructor.getGenericExceptionTypes().length != 0)
                throw new AssertionError("original constructor acquired declared Exceptions: " + constructor);
        }
        if (typed.getGenericParameterTypes()[0] != Ordered.class
                || bridge.getGenericParameterTypes()[0] != Object.class
                || Receiver.class.getMethod("compare", Ordered.class).getDeclaringClass() != Ordered.class
                || Receiver.class.getMethod("compare", Object.class).getDeclaringClass() != Ordered.class)
            throw new AssertionError("default inherited declaration changed");
        for (Method method : Receiver.class.getDeclaredMethods()) {
            if (method.getName().equals("compare"))
                throw new AssertionError("concrete receiver acquired an invented comparison");
        }
    }
    public static void main(String[] args) throws Exception {
        Method typed = Ordered.class.getDeclaredMethod("compare", Ordered.class);
        Method bridge = Ordered.class.getDeclaredMethod("compare", Object.class);
        Method root = Ordering.class.getDeclaredMethod("compare", Object.class);
        metadata(typed, bridge, root);
        Receiver left = new Receiver(); left.tag = 2;
        Receiver right = new Receiver(); right.tag = 3;
        for (int pass = 0; pass < 3; pass++) {
            if (pass == 2) {
                Offset dynamicLeft = new Offset(); dynamicLeft.offset = 20; dynamicLeft.tag = 2;
                Offset dynamicRight = new Offset(); dynamicRight.offset = -2; dynamicRight.tag = 3;
                left = dynamicLeft; right = dynamicRight;
            }
            left.value = pass == 1 ? -3 : 9;
            right.value = pass == 1 ? 7 : 4;
            int expected = pass == 0 ? 5 : pass == 1 ? -10 : 27;
            for (Call call : paths(left, right, typed, bridge, root))
                execute(call, left, right, left, right, expected, 1234, 1, 1, 1, null, null);
        }
        RuntimeException problem = new IllegalStateException("owned comparison failure");
        Trace.failure = problem;
        for (Call call : paths(left, right, typed, bridge, root))
            execute(call, left, right, left, right, 0, 1, 1, 0, 0, IllegalStateException.class, problem);
        Trace.failure = null;
        left.failure = problem;
        for (Call call : paths(left, right, typed, bridge, root))
            execute(call, left, right, left, right, 0, 12, 1, 1, 0, IllegalStateException.class, problem);
        left.failure = null;
        right.failure = problem;
        for (Call call : paths(left, right, typed, bridge, root))
            execute(call, left, right, left, right, 0, 123, 1, 1, 1, IllegalStateException.class, problem);
        right.failure = null;
        for (Call call : paths(left, null, typed, bridge, root))
            execute(call, left, right, left, null, 0, 12, 1, 1, 0, NullPointerException.class, null);
        for (Call call : paths(null, right, typed, bridge, root))
            execute(call, left, right, null, null, 0, 0, 0, 0, 0, NullPointerException.class, null);
        Trace.failure = problem;
        for (Call call : erased(left, new Object(), bridge, root))
            execute(call, left, right, null, null, 0, 0, 0, 0, 0, ClassCastException.class, null);
        Trace.failure = null;
        System.out.println("values=" + values + ",identities=" + identities
                + ",states=" + states + ",failures=" + failures);
        System.out.println("source-default-owner=retained,original-parameter-bridge=retained,virtual-order-and-cast-order=matched");
    }
}
