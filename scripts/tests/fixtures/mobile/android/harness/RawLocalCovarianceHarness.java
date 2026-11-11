import fixture.*;
import kotlin.jvm.functions.Function1;
import kotlin.jvm.internal.FunctionBase;
import kotlin.jvm.internal.DefaultConstructorMarker;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class RawLocalCovarianceHarness {
    public static final class Incoming implements Function1<Object,Object> {
        public Object invoke(Object value) { Trace.hit(4); Trace.seen = value; return value; }
    }
    static void require(boolean ok) { if (!ok) throw new AssertionError("raw local covariance contract"); }
    static Object call(int route, Function1<Object,Object> callback, Object value) throws Throwable {
        if (route == 0) return callback.invoke(value);
        if (route == 1) return RawLocalCovarianceCaller.call(callback, value);
        if (route == 2) {
            try { Method method = callback.getClass().getDeclaredMethod("invoke", Object.class); method.setAccessible(true); return method.invoke(callback, value); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return MethodHandles.privateLookupIn(callback.getClass(), MethodHandles.lookup()).findVirtual(callback.getClass(),
                "invoke", MethodType.methodType(Object.class, Object.class)).invoke(callback, value);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        boolean classMode = Boolean.parseBoolean(args[2]);
        boolean defaults = mode != 1 && mode != 3 && mode != 6;
        Object token = mode == 2 || mode == 3 ? null : new Object();
        Incoming incoming = new Incoming();
        Trace.fail = mode == 4 ? 2 : mode == 5 ? 3 : mode == 6 ? 4 : 0;
        Trace.failure = new IllegalStateException("owned raw covariance failure");
        Function1<Object,Object> previous = null;
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.partial = null; Trace.seen = null;
            Owner<Object> owner = null; Throwable failure = null;
            try { owner = defaults ? new Owner<>() : new Owner<>(incoming); }
            catch (Throwable caught) { failure = caught; }
            require(Trace.events == 2 && Trace.partial != null);
            if (mode == 4) { require(failure == Trace.failure); }
            else {
                require(failure == null && owner == Trace.partial);
                Function1<Object,Object> callback = (Function1<Object,Object>)owner.callback;
                require(defaults ? previous == null || callback == previous : callback == incoming);
                if (defaults) {
                    require((callback instanceof FunctionBase) == classMode);
                    if (classMode) require(((FunctionBase)callback).getArity() == 1);
                }
                previous = callback;
                Object result = new Object(); failure = null;
                try { result = call(route, callback, token); } catch (Throwable caught) { failure = caught; }
                if (defaults && token == null) require(failure instanceof NullPointerException && Trace.events == 2 && Trace.seen == null);
                else {
                    long events = defaults ? 23 : 24;
                    require(Trace.events == events);
                    if (mode == 5 || mode == 6) require(failure == Trace.failure && Trace.seen == null);
                    else require(failure == null && result == (defaults ? null : token) && Trace.seen == token);
                }
            }
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
