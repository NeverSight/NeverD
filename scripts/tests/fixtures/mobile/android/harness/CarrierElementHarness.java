import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class CarrierElementHarness {
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("carrier element identity, effects or cast timing changed");
    }
    static Object call(Factory<?> factory, long flags, int variant, int route) throws Throwable {
        if (route == 0) return variant == 0 ? Client.element(factory, flags) : Client.text(factory, flags);
        if (route == 1) return variant == 0 ? CarrierElementCaller.element(factory, flags) : CarrierElementCaller.text(factory, flags);
        if (route == 2) {
            try { return Client.class.getMethod(variant == 0 ? "element" : "text", Factory.class, long.class).invoke(null, factory, flags); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        if (variant == 0) return (Object)MethodHandles.lookup().findStatic(Client.class, "element",
                MethodType.methodType(Object.class, Factory.class, long.class)).invokeExact((Factory)factory, flags);
        return (String)MethodHandles.lookup().findStatic(Client.class, "text",
                MethodType.methodType(String.class, Factory.class, long.class)).invokeExact((Factory)factory, flags);
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        Trace.failure = new RuntimeException("owned carrier element failure");
        Trace.failAt = mode == 4 ? 6 : mode >= 1 && mode <= 3 ? mode : 0;
        int[] mutable = {0};
        Object value = mode == 6 ? null : mode == 7 ? mutable : new String("owned element");
        Factory<Object> producer = () -> {
            Trace.reads++; Trace.hit(3);
            if (value == mutable) mutable[0]++;
            return value;
        };
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0;
            int oldReads = Trace.reads, oldMutable = mutable[0];
            long flags = attempt == 0 ? Long.MIN_VALUE : Long.MAX_VALUE;
            Object result = null; Throwable failure = null;
            try { result = call(mode == 5 ? null : producer, flags, variant, route); }
            catch (Throwable error) { failure = error; }
            String expected = mode == 1 ? "" : mode == 2 || mode == 5 ? "2" : mode == 3 ? "23" : "236";
            if (attempt == 0) expected = "1" + expected;
            require(Trace.events == (expected.isEmpty() ? 0 : Long.parseLong(expected)));
            if (mode == 1) {
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (mode >= 2 && mode <= 4) require(failure == Trace.failure);
            else if (mode == 5) require(failure instanceof NullPointerException);
            else if (mode == 7 && variant == 1) require(failure instanceof ClassCastException);
            else require(failure == null && result == value);
            require(Trace.reads == oldReads + (mode != 1 && mode != 2 && mode != 5 ? 1 : 0));
            require(mutable[0] == oldMutable + (mode == 7 ? 1 : 0));
            require(Trace.boxes == 0 && Trace.flags == (mode == 1 ? 0 : flags));
            System.out.println("variant="+variant+",route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
