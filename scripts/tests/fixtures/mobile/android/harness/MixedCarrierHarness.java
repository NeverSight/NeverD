import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class MixedCarrierHarness {
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("mixed carrier arrays, identity or effect order changed");
    }
    static Object call(Object[] inputs, Saver<?,?> saver, Factory<?> producer, long flags, int route) throws Throwable {
        if (route == 0) return Client.call(inputs, saver, producer, flags);
        if (route == 1) return MixedCarrierCaller.call(inputs, saver, producer, flags);
        if (route == 2) {
            try { return Client.class.getMethod("call", Object[].class, Saver.class, Factory.class, long.class).invoke(null, inputs, saver, producer, flags); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        return (Object)MethodHandles.lookup().findStatic(Client.class, "call",
                MethodType.methodType(Object.class, Object[].class, Saver.class, Factory.class, long.class)).invokeExact(inputs, (Saver)saver, (Factory)producer, flags);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Trace.failure = new RuntimeException("owned mixed carrier failure");
        Trace.failAt = mode == 4 ? 7 : mode == 5 ? 6 : mode >= 1 && mode <= 3 ? mode : 0;
        int[] value = mode == 10 ? null : new int[]{0};
        Object key = new Object(), replacement = new Object(), untouched = new Object();
        Object saved = mode == 11 ? null : new Object();
        Object[] inputs = mode == 6 ? null : mode == 7 ? new Object[0] : new Object[]{key};
        Object[] untouchedInputs = new Object[0];
        Factory<int[]> producer = () -> {
            Trace.reads++; Trace.hit(3);
            inputs[0] = replacement;
            if (value != null) value[0]++;
            return value;
        };
        Saver<int[],Object> saver = incoming -> {
            Trace.saves++; Trace.hit(7); require(incoming == value);
            return saved;
        };
        Saver<String,Object> wrongSaver = incoming -> { throw new AssertionError("missing saver cast"); };
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.inputs = untouchedInputs; Trace.key = untouched; Trace.saved = untouched;
            if (inputs != null && inputs.length != 0) inputs[0] = key;
            int oldReads = Trace.reads, oldSaves = Trace.saves, oldValue = value == null ? 0 : value[0];
            long flags = attempt == 0 ? Long.MIN_VALUE : Long.MAX_VALUE;
            Object result = null; Throwable failure = null;
            try { result = call(inputs, mode == 9 ? null : mode == 12 ? wrongSaver : saver, mode == 8 ? null : producer, flags, route); }
            catch (Throwable error) { failure = error; }
            String expected = mode == 1 ? "" : mode == 2 || mode == 6 || mode == 7 || mode == 8 ? "2" :
                    mode == 3 || mode == 9 || mode == 12 ? "23" : mode == 4 ? "237" : "2376";
            if (attempt == 0) expected = "1" + expected;
            require(Trace.events == (expected.isEmpty() ? 0 : Long.parseLong(expected)));
            if (mode == 1) {
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (mode >= 2 && mode <= 5) require(failure == Trace.failure);
            else if (mode == 6 || mode == 8 || mode == 9) require(failure instanceof NullPointerException);
            else if (mode == 7) require(failure instanceof ArrayIndexOutOfBoundsException);
            else if (mode == 12) require(failure instanceof ClassCastException);
            else require(failure == null && result == value);
            boolean read = mode != 1 && mode != 2 && mode != 6 && mode != 7 && mode != 8;
            boolean readCompleted = read && mode != 3;
            boolean save = readCompleted && mode != 9 && mode != 12;
            boolean saveCompleted = save && mode != 4;
            require(Trace.reads == oldReads + (read ? 1 : 0) && Trace.saves == oldSaves + (save ? 1 : 0));
            require(Trace.inputs == (mode == 1 || mode == 2 ? untouchedInputs : inputs));
            require(Trace.key == (mode == 1 || mode == 2 || mode == 6 || mode == 7 ? untouched : key));
            require(Trace.saved == (saveCompleted ? saved : untouched));
            require(Trace.flags == (mode == 1 ? 0 : flags));
            if (inputs != null && inputs.length != 0) require(inputs[0] == (readCompleted ? replacement : key));
            if (value != null) require(value[0] == oldValue + (readCompleted ? 1 : 0));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
