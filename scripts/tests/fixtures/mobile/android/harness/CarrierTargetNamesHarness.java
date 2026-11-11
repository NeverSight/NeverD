import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class CarrierTargetNamesHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("local carrier name contract"); }
    static Object call(Function target, Object first, Object second, int route) throws Throwable {
        try {
            if (route == 0) return target.invoke(first, second);
            if (route == 1) return CarrierTargetNamesCaller.call(target, first, second);
            if (route == 2) return Function.class.getMethod("invoke", Object.class, Object.class).invoke(target, first, second);
            MethodHandle method = MethodHandles.publicLookup().findVirtual(Function.class, "invoke",
                MethodType.methodType(Object.class, Object.class, Object.class));
            return (Object) method.invokeExact(target, first, second);
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Function target = Owner.make();
        Method primitive = target.getClass().getDeclaredMethod(args[2], long.class, long.class);
        primitive.setAccessible(true);
        MethodHandle primitiveHandle = MethodHandles.lookup().unreflect(primitive);
        RuntimeException problem = new IllegalStateException("owned name failure");
        int trace = 0;
        for (int attempt = 0; attempt < 2; attempt++) {
            long a = attempt == 0 ? Long.MIN_VALUE : Long.MAX_VALUE;
            long b = attempt == 0 ? 0x123456789abcdefL : -17L;
            CarrierA first = new CarrierA(a);
            CarrierB second = new CarrierB(b);
            Object left = mode == 1 ? "wrong" : mode == 2 ? null : first;
            Object right = mode == 3 ? "wrong" : mode == 4 ? null : second;
            Trace.firstFailure = mode == 5 ? problem : null;
            Trace.secondFailure = mode == 6 ? problem : null;
            Trace.targetFailure = mode == 7 ? problem : null;
            Trace.boxFailure = mode == 8 ? problem : null;
            Trace.events = 0;
            Throwable caught = null;
            try {
                Object result = call(target, left, right, route);
                require(mode == 0 && result != second && ((CarrierB) result).value == b);
                require(Trace.first == a && Trace.second == b);
            } catch (Throwable failure) { caught = failure; }
            if (mode == 0) require(caught == null);
            else if (mode <= 4) require(caught != null && caught.getClass() == (mode % 2 == 1 ? ClassCastException.class : NullPointerException.class));
            else require(caught == problem);
            int expected = mode == 1 || mode == 2 ? 0 : mode == 3 || mode == 4 || mode == 5 ? 1 : mode == 6 ? 12 : mode == 7 ? 123 : 1234;
            require(Trace.events == expected);
            trace = trace * 10 + Trace.events;
            Trace.targetFailure = null; Trace.events = 0;
            Object value = primitive.invoke(target, b, a);
            require(((Long) value).longValue() == a && Trace.first == b && Trace.second == a && Trace.events == 3);
            Trace.events = 0;
            require(((Long) primitiveHandle.invokeWithArguments(target, a, b)).longValue() == b && Trace.events == 3);
            Trace.targetFailure = problem;
            try { primitive.invoke(target, a, b); throw new AssertionError("target failure absent"); }
            catch (InvocationTargetException failure) { require(failure.getCause() == problem); }
            Trace.targetFailure = null;
        }
        System.out.println("route=" + route + ",mode=" + mode + ",attempts=2,trace=" + trace);
    }
}
