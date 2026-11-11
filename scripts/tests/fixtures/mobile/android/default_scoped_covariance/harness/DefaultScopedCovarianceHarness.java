import fixture.*;
import kotlin.jvm.functions.Function1;
import kotlin.jvm.internal.FunctionBase;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class DefaultScopedCovarianceHarness {
    public static final class Incoming implements Function1<Object,Object> {
        public Object invoke(Object value) { Trace.hit(4); Trace.seen = value; return value; }
    }
    static void require(boolean value, String detail) { if (!value) throw new AssertionError("default scoped covariance contract: " + detail); }
    static Function1<Object,Object> factory(int route, int mode, Incoming incoming) throws Throwable {
        boolean defaults = mode != 1 && mode != 3 && mode != 6 && mode != 7 && mode != 8;
        if (!defaults && mode != 7 && mode != 8) { Callbacks.make(incoming); return (Function1<Object,Object>) Trace.callback; }
        int mask = mode == 7 ? 0 : mode == 8 ? 2 : 1;
        if (defaults && route % 3 == 0) return (Function1<Object,Object>) FrozenCallbacks.originalDefault();
        if (route % 3 == 1) {
            try { Callbacks.class.getMethod("make$default", Function1.class, int.class, Object.class).invoke(null, null, mask, null); return (Function1<Object,Object>) Trace.callback; }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        MethodHandles.publicLookup().findStatic(Callbacks.class, "make$default",
            MethodType.methodType(void.class, Function1.class, int.class, Object.class)).invokeExact((Function1)null, mask, (Object)null);
        return (Function1<Object,Object>) Trace.callback;
    }
    static Object call(int route, Function1<Object,Object> callback, Object value) throws Throwable {
        if (route == 0) return callback.invoke(value);
        if (route == 1) return DefaultScopedCovarianceCaller.call(callback, value);
        if (route == 2 || route == 4) {
            Method selected = null;
            for (Method method : callback.getClass().getDeclaredMethods())
                if (method.getName().equals("invoke") && method.getReturnType() == (route == 4 ? Void.class : Object.class)) selected = method;
            require(selected != null, "original reflection entry"); selected.setAccessible(true);
            try { return selected.invoke(callback, value); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        return MethodHandles.privateLookupIn(callback.getClass(), MethodHandles.lookup()).findVirtual(callback.getClass(), "invoke",
            MethodType.methodType(route == 5 ? Void.class : Object.class, Object.class)).invoke(callback, value);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        boolean classMode = Boolean.parseBoolean(args[2]), original = args[3].equals("original");
        Method defaultMethod = Callbacks.class.getMethod("make$default", Function1.class, int.class, Object.class);
        require(defaultMethod.isSynthetic() == original, "reported default flag");
        TypeVariable<Method>[] formals = Callbacks.class.getMethod("make", Function1.class).getTypeParameters();
        require(formals.length == 1 && formals[0].getName().equals("K") && formals[0].getBounds().length == 1
            && formals[0].getBounds()[0] == Object.class, "original method formal");
        boolean defaults = mode != 1 && mode != 3 && mode != 6 && mode != 7 && mode != 8;
        Object token = mode == 2 || mode == 3 ? null : new Object();
        Incoming incoming = new Incoming();
        Trace.fail = mode == 4 ? 2 : mode == 5 ? 3 : mode == 6 ? 4 : 0;
        Trace.failure = new IllegalStateException("owned default scope failure");
        Function1<Object,Object> previous = null;
        for (int attempt = 0; attempt < 2; attempt++) {
            Object sentinel = new Object(); Trace.events = 0; Trace.callback = Trace.seen = sentinel;
            Function1<Object,Object> callback = null; Throwable failure = null;
            try { callback = factory(route, mode, incoming); } catch (Throwable error) { failure = error; }
            if (mode == 7 || mode == 8) require(failure instanceof NullPointerException && Trace.events == 0 && Trace.callback == sentinel, "unselected null argument");
            else {
                require(Trace.events == 2 && Trace.callback != sentinel && Trace.callback != null, "factory store and call order");
                if (mode == 4) require(failure == Trace.failure, "factory failure identity");
                else {
                    require(failure == null && callback == Trace.callback, "factory callback identity");
                    require(defaults ? previous == null || callback == previous : callback == incoming, "singleton and incoming identity");
                    if (defaults) {
                        require((callback instanceof FunctionBase) == classMode, "original superclass");
                        if (classMode) require(((FunctionBase)callback).getArity() == 1, "original arity");
                        Method bridge = callback.getClass().getDeclaredMethod("invoke", Object.class);
                        for (Method m : callback.getClass().getDeclaredMethods()) if (m.getName().equals("invoke") && m.getReturnType() == Object.class) bridge = m;
                        require(bridge.isBridge() && bridge.isSynthetic(), "regenerated original bridge flags");
                    }
                    previous = callback;
                    Object result = sentinel; failure = null;
                    try { result = call(route, mode == 9 ? null : callback, token); } catch (Throwable error) { failure = error; }
                    if (mode == 9 || defaults && token == null) require(failure instanceof NullPointerException && Trace.events == 2 && Trace.seen == sentinel, "checked null precedence");
                    else {
                        require(Trace.events == (defaults ? 23 : 24), "callback event order");
                        if (mode == 5 || mode == 6) require(failure == Trace.failure && Trace.seen == sentinel, "target failure and partial effects");
                        else require(failure == null && result == (defaults ? null : token) && Trace.seen == token, "result and argument identity");
                    }
                }
            }
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
