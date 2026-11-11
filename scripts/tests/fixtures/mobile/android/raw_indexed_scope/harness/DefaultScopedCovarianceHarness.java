import fixture.*;
import kotlin.jvm.functions.Function2;
import kotlin.jvm.internal.FunctionBase;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class DefaultScopedCovarianceHarness {
    public static final class Incoming implements Function2<Object,Object,Object> {
        public Object invoke(Object index, Object value) { Trace.hit(4); Trace.index = 91; Trace.seen = value; return value; }
    }
    public static final class ObservedNumber extends Number {
        public int intValue() { Trace.unboxes++; Trace.hit(1); return 37; }
        public long longValue() { throw new AssertionError("wrong conversion"); }
        public float floatValue() { throw new AssertionError("wrong conversion"); }
        public double doubleValue() { throw new AssertionError("wrong conversion"); }
    }
    static void require(boolean value, String detail) { if (!value) throw new AssertionError("default scoped covariance contract: " + detail); }
    static Function2<Object,Object,Object> factory(int route, int mode, Incoming incoming) throws Throwable {
        if (mode == 15) { Callbacks.make(incoming); return (Function2<Object,Object,Object>) Trace.callback; }
        if (route % 3 == 0) return FrozenCallbacks.originalDefault();
        if (route % 3 == 1) {
            try { Callbacks.class.getMethod("make$default", Function2.class, int.class, Object.class).invoke(null, null, 1, null); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        } else MethodHandles.publicLookup().findStatic(Callbacks.class, "make$default",
            MethodType.methodType(void.class, Function2.class, int.class, Object.class)).invokeExact((Function2)null, 1, (Object)null);
        return (Function2<Object,Object,Object>) Trace.callback;
    }
    static Object call(int route, Function2<Object,Object,Object> callback, Object index, Object value) throws Throwable {
        if (route == 0) return callback.invoke(index, value);
        if (route == 1) return DefaultScopedCovarianceCaller.call(callback, index, value);
        if (route == 2 || route == 4) {
            Method method = callback.getClass().getDeclaredMethod("invoke", route == 4 ? int.class : Object.class, Object.class);
            method.setAccessible(true);
            Object argument = route == 4 ? Integer.valueOf(((Number)index).intValue()) : index;
            try { return method.invoke(callback, argument, value); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        MethodHandle handle = MethodHandles.privateLookupIn(callback.getClass(), MethodHandles.lookup()).findVirtual(callback.getClass(), "invoke",
            MethodType.methodType(route == 5 ? Void.class : Object.class, route == 5 ? int.class : Object.class, Object.class));
        return route == 5 ? handle.invoke(callback, ((Number)index).intValue(), value) : handle.invoke(callback, index, value);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        boolean original = args[3].equals("original");
        require(!Boolean.parseBoolean(args[2]), "raw Object superclass profile");
        Method defaultMethod = Callbacks.class.getMethod("make$default", Function2.class, int.class, Object.class);
        require(defaultMethod.isSynthetic() == original, "reported default flag");
        TypeVariable<Method>[] formals = Callbacks.class.getMethod("make", Function2.class).getTypeParameters();
        require(formals.length == 1 && formals[0].getName().equals("K") && formals[0].getBounds().length == 1
            && formals[0].getBounds()[0] == Object.class, "original method formal");
        Object index = mode == 0 ? Integer.valueOf(17) : mode == 1 ? Long.valueOf(0x100000021L)
            : mode == 2 ? Double.valueOf(Double.NaN) : mode == 3 ? Double.valueOf(Double.POSITIVE_INFINITY)
            : mode == 4 ? Double.valueOf(Double.NEGATIVE_INFINITY) : mode == 5 ? Double.valueOf(-17.9)
            : mode == 6 ? Float.valueOf(-0.0f) : mode == 8 || mode == 15 ? new Object()
            : mode == 9 ? null : new ObservedNumber();
        int expectedIndex = mode == 0 ? 17 : mode == 1 ? 33 : mode == 2 || mode == 6 ? 0
            : mode == 3 ? Integer.MAX_VALUE : mode == 4 ? Integer.MIN_VALUE : mode == 5 ? -17 : mode == 15 ? 91 : 37;
        Object value = mode == 10 ? null : new Object(); Incoming incoming = new Incoming();
        Trace.fail = mode == 12 ? 1 : mode == 13 ? 3 : mode == 14 ? 2 : 0;
        Trace.failure = new IllegalStateException("owned indexed scope failure");
        Function2<Object,Object,Object> previous = null;
        for (int attempt = 0; attempt < 2; attempt++) {
            Object sentinel = new Object(); Trace.events = 0; Trace.unboxes = 0; Trace.index = 0x1357;
            Trace.callback = Trace.seen = sentinel;
            Function2<Object,Object,Object> callback = null; Throwable failure = null;
            try { callback = factory(route, mode, incoming); } catch (Throwable error) { failure = error; }
            require(Trace.events == 2 && Trace.callback != sentinel && Trace.callback != null, "factory partial store");
            if (mode == 14) require(failure == Trace.failure && Trace.unboxes == 0 && Trace.seen == sentinel && Trace.index == 0x1357, "factory failure identity");
            else {
                require(failure == null && callback == Trace.callback, "factory callback identity");
                require(mode == 15 ? callback == incoming : previous == null || callback == previous, "original singleton identity");
                previous = callback;
                if (mode != 15) {
                    require(!(callback instanceof FunctionBase), "original raw superclass");
                    Method bridge = callback.getClass().getDeclaredMethod("invoke", Object.class, Object.class);
                    Method target = callback.getClass().getDeclaredMethod("invoke", int.class, Object.class);
                    require(bridge.isBridge() == original && bridge.isSynthetic() == original, "reported bridge flags");
                    require(target.getReturnType() == Void.class && Modifier.isFinal(target.getModifiers()), "original typed descriptor");
                }
                Object result = sentinel; failure = null;
                try { result = call(route, mode == 11 ? null : callback, index, value); } catch (Throwable error) { failure = error; }
                boolean observed = (mode == 7 || mode == 10 || mode == 12 || mode == 13);
                int unboxes = observed ? 1 : 0;
                long trace = mode == 8 || mode == 9 || mode == 11 ? 2 : mode == 10 || mode == 12 ? 21 : mode == 7 || mode == 13 ? 213 : mode == 15 ? 24 : 23;
                require(Trace.events == trace && Trace.unboxes == unboxes, "cast/unbox/target order and count");
                boolean failed = mode >= 8 && mode <= 13;
                if (mode == 8) require(failure instanceof ClassCastException, "Number cast failure");
                else if (mode == 9 || mode == 10 || mode == 11) require(failure instanceof NullPointerException, "null precedence");
                else if (mode == 12 || mode == 13) require(failure == Trace.failure, "exact conversion/target failure");
                else require(failure == null && result == (mode == 15 ? value : null), "original result");
                require(Trace.seen == (failed ? sentinel : value) && Trace.index == (failed ? 0x1357 : expectedIndex), "argument identity, integer result and partial stores");
            }
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
