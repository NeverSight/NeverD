import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ErasedPairHarness {
    static final class Transformer implements Transform<Object, Object> {
        Object value, output;
        public Object input() { Trace.inputs++; Trace.hit(3); return value; }
        public Object decode(Object input) { Trace.decoded++; Trace.input = input; Trace.hit(4); return output; }
    }
    static final class Consumer implements Callback<String> {
        public void accept(String value) { Trace.callbacks++; Trace.output = value; Trace.hit(5); }
    }
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("independent generic formals and original call effects");
    }
    static Box call(Root receiver, Transform transform, Callback callback, long flags, int route, int flavor) throws Throwable {
        if (route == 0) return flavor == 0 ? Client.call(receiver, transform, callback, flags) : Client.iface(receiver, transform, callback, flags);
        if (route == 1) return flavor == 0 ? ErasedPairCaller.call(receiver, transform, callback, flags) : ErasedPairCaller.iface(receiver, transform, callback, flags);
        String name = flavor == 0 ? "call" : "iface";
        Class<?> owner = flavor == 0 ? Root.class : API.class;
        if (route == 2) {
            try { return (Box) Client.class.getMethod(name, owner, Transform.class, Callback.class, long.class).invoke(null, receiver, transform, callback, flags); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        java.lang.invoke.MethodHandle handle = MethodHandles.lookup().findStatic(Client.class, name,
            MethodType.methodType(Box.class, owner, Transform.class, Callback.class, long.class));
        return flavor == 0 ? (Box) handle.invokeExact(receiver, transform, callback, flags)
                           : (Box) handle.invokeExact((API) receiver, transform, callback, flags);
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]);
        int mode = Integer.parseInt(args[2]), flavor = Integer.parseInt(args[3]);
        Transformer transformer = new Transformer();
        transformer.value = variant == 1 ? null : new Object();
        transformer.output = variant == 2 ? null : variant == 3 ? new Object() : "owned output";
        Consumer consumer = new Consumer();
        RuntimeException original = new IllegalArgumentException("owned pair failure");
        Trace.failure = original; Trace.failAt = mode;
        long events = 0;
        int inputs = 0, decoded = 0, callbacks = 0, boxes = 0;
        Box previous = null;
        for (int attempt = 1; attempt <= 2; attempt++) {
            long flags = attempt == 1 ? Long.MIN_VALUE : Long.MAX_VALUE;
            int failureStage = 0;
            if (variant != 6) {
                if (attempt == 1) events = 1;
                if (mode == 1) failureStage = 1;
                else for (int stage = 2; stage <= 7; stage++) {
                    events = events * 10 + stage;
                    if (stage == 3) inputs++;
                    if (stage == 4) decoded++;
                    if (stage == 5) callbacks++;
                    if (stage == 7) boxes++;
                    if (mode == stage || stage == 2 && variant == 4 || stage == 4 && (variant == 3 || variant == 5)) {
                        failureStage = stage; break;
                    }
                }
            }
            Root receiver = null;
            try {
                if (variant != 6) receiver = new Root();
                Box result = call(receiver, variant == 4 ? null : transformer, variant == 5 ? null : consumer, flags, route, flavor);
                require(failureStage == 0 && variant != 6 && result != previous && result.value == transformer.value);
                previous = result;
            } catch (Throwable error) {
                if (mode == 1) {
                    if (attempt == 1) require(error.getClass() == ExceptionInInitializerError.class && error.getCause() == original);
                    else require(error.getClass() == NoClassDefFoundError.class);
                } else if (failureStage != 0 && mode == failureStage) require(error == original);
                else require(variant >= 3 && error.getClass() == (variant == 3 ? ClassCastException.class : NullPointerException.class));
            }
            require(Trace.events == events && Trace.inputs == inputs && Trace.decoded == decoded && Trace.callbacks == callbacks && Trace.boxes == boxes);
            require(Trace.flags == (mode == 1 ? 0 : flags));
            require(Trace.receiver == receiver);
            if (decoded > 0) require(Trace.input == transformer.value);
            if (callbacks > 0) require(Trace.output == transformer.output);
        }
        System.out.println("variant=" + variant + ",route=" + route + ",mode=" + mode + ",flavor=" + flavor + ",trace=" + Trace.events);
    }
}
