import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class ProtectedScalarArrayTargetsHarness {
    private static int values, identities, states, failures;
    private static Method typed, bridge, parent;
    private static MethodHandle typedHandle, bridgeHandle, localHandle;
    private static void same(Object expected, Object actual) {
        if (expected != actual) throw new AssertionError("array bridge identity changed");
        identities++;
    }
    private static void state(int expected, int actual) {
        if (expected != actual) throw new AssertionError("array bridge state changed " + expected + ":" + actual);
        states++;
    }
    private static void value(int expected, int actual) {
        if (expected != actual) throw new AssertionError("array bridge result changed");
        values++;
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static int call(int route, Base receiver, Object argument) throws Throwable {
        try {
            switch (route) {
                case 0: return receiver.call(argument);
                case 1: return Caller.call(receiver, argument);
                case 2: return (Integer) typed.invoke(receiver, argument);
                case 3: return (Integer) bridge.invoke(receiver, argument);
                case 4: return (Integer) parent.invoke(receiver, argument);
                case 5: return (int) typedHandle.invokeExact((Object) receiver, argument);
                case 6: return (int) bridgeHandle.invokeExact((Object) receiver, argument);
                case 7: return (int) localHandle.invokeExact((Object) receiver, argument);
                default: throw new AssertionError();
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void reset(Base<?> receiver, int[] marker) {
        receiver.calls = receiver.callbacks = 0; receiver.seen = null; receiver.failure = null; marker[0] = 7;
    }
    private static void run(int route, Class<?> owner, Object input) throws Throwable {
        Base<?> core = (Base<?>) owner.getConstructor().newInstance(); int[] marker = {7}; core.marks = marker;
        value(12, call(route, core, input)); same(input, core.seen);
        value(15, call(route, core, input)); same(input, core.seen); same(marker, core.marks);
        state(2, core.calls); state(route < 2 ? 2 : 0, core.callbacks); state(13, marker[0]);
        marker[0] = 30;
        value(35, call(route, core, input)); same(input, core.seen);
        state(3, core.calls); state(route < 2 ? 3 : 0, core.callbacks); state(33, marker[0]);
        for (int mode = 0; mode < 4; mode++) {
            reset(core, marker);
            RuntimeException failure = new RuntimeException("owned-array");
            if (mode == 2) core.failure = failure;
            Object argument = mode == 0 ? null : mode == 1 ? new Object[2] : input;
            Base<?> receiver = mode == 3 ? null : core;
            try { call(route, receiver, argument); throw new AssertionError("missing array bridge failure"); }
            catch (Throwable error) {
                Object wanted = mode == 2 ? failure : mode == 1
                    ? route == 2 ? IllegalArgumentException.class : ClassCastException.class : NullPointerException.class;
                same(wanted, mode == 2 ? error : error.getClass()); failures++;
            }
            int entered = mode == 0 || mode == 2 ? 1 : 0;
            state(entered, core.calls); state(route < 2 && mode != 3 ? 1 : 0, core.callbacks);
            state(entered == 1 ? 10 : 7, marker[0]);
            same(mode == 2 ? input : null, core.seen); same(marker, core.marks);
        }
    }
    public static void main(String[] args) throws Throwable {
        String codes = "ZBSCIJFD";
        Class<?>[] components = {boolean.class, byte.class, short.class, char.class, int.class, long.class, float.class, double.class};
        for (int i = 0; i < codes.length(); i++) {
            Class<?> owner = Class.forName("fixture.Core" + codes.charAt(i));
            Object input = Array.newInstance(components[i], 2);
            typed = owner.getDeclaredMethod("size", input.getClass());
            bridge = owner.getDeclaredMethod("size", Object.class);
            parent = Base.class.getDeclaredMethod("size", Object.class);
            if (!Modifier.isFinal(owner.getModifiers()) || typed.isBridge() || typed.isSynthetic()
                    || !bridge.isBridge() || !bridge.isSynthetic() || !Modifier.isPublic(bridge.getModifiers())
                    || (args[0].equals("original") ? !Modifier.isProtected(typed.getModifiers()) : !Modifier.isPublic(typed.getModifiers()))
                    || typed.getReturnType() != int.class || bridge.getReturnType() != int.class
                    || typed.getExceptionTypes().length != 0 || bridge.getExceptionTypes().length != 0
                    || ((ParameterizedType) owner.getGenericSuperclass()).getActualTypeArguments()[0] != input.getClass())
                throw new AssertionError("original array contract or reported access changed");
            typed.setAccessible(true); parent.setAccessible(true);
            MethodType erased = MethodType.methodType(int.class, Object.class, Object.class);
            typedHandle = MethodHandles.lookup().unreflect(typed).asType(erased);
            bridgeHandle = MethodHandles.lookup().findVirtual(owner, "size", MethodType.methodType(int.class, Object.class)).asType(erased);
            localHandle = MethodHandles.lookup().unreflect(owner.getDeclaredMethod("local", input.getClass())).asType(erased);
            for (int route = 0; route < 8; route++) run(route, owner, input);
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("all-primitive-array-bridges=retained,typed-access-difference=reported,original-parent-and-caller-dispatch=retained");
    }
}
