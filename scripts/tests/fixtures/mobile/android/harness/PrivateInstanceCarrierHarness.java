import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class PrivateInstanceCarrierHarness {
    static final int FLAGS = Integer.MIN_VALUE + 37;
    static final long BITS = 0xfff8123456789abcL;
    static void require(boolean value) { if (!value) throw new AssertionError("private carrier invocation changed"); }
    static final class Maker implements Producer<Object> {
        Object result, seen; int calls, flags; long bits;
        public Object get(int flag, Object input, long raw) {
            calls++; Trace.hit(5); seen = input; flags = flag; bits = raw;
            if (input != null) ((int[]) input)[0] += 10;
            return result;
        }
    }
    static void call(Provider target, boolean present, Object input, Producer producer, int route) throws Throwable {
        if (route == 0) { target.call(present, FLAGS, input, producer, BITS); return; }
        if (route == 1) { PrivateInstanceCarrierCaller.call(target, present, FLAGS, input, producer, BITS); return; }
        if (route == 2) {
            try { Provider.class.getMethod("call", boolean.class, int.class, Object.class, Producer.class, long.class)
                    .invoke(target, present, FLAGS, input, producer, BITS); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
            return;
        }
        MethodHandles.publicLookup().findVirtual(Provider.class, "call",
                MethodType.methodType(void.class, boolean.class, int.class, Object.class, Producer.class, long.class))
                .invokeExact(target, present, FLAGS, input, producer, BITS);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Class<?> carrier = Class.forName("fixture.Provider$Carrier", false, Provider.class.getClassLoader());
        require(Modifier.isPrivate(carrier.getModifiers()) && Modifier.isStatic(carrier.getModifiers()));
        Method method = Provider.class.getDeclaredMethod("dispatch", int.class, Object.class, carrier, long.class);
        require(method.getModifiers() == (Modifier.PRIVATE | Modifier.FINAL));
        require(method.getReturnType() == void.class && method.getTypeParameters().length == 1);
        TypeVariable<?> formal = method.getTypeParameters()[0];
        require(formal.getBounds()[0] == Object.class);
        require(((ParameterizedType) method.getGenericParameterTypes()[2]).getActualTypeArguments()[0].equals(formal));
        int[] failures = {0, 1, 9, 2, 4, 5, 6, 7, 8};
        Trace.failAt = mode < failures.length ? failures[mode] : 0;
        Trace.failure = new RuntimeException("owned private carrier failure");
        Provider target = null;
        Maker maker = new Maker();
        int[] inputArray = {11}, output = {31};
        Object input = mode == 13 ? null : inputArray;
        maker.result = mode == 11 ? null : mode == 12 ? new Object() : mode == 15 ? new int[0] : mode == 16 ? input : output;
        boolean present = mode != 9;
        Producer producer = mode == 10 ? null : maker;
        Object sentinel = new Object();
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.seen = sentinel; Trace.receiver = sentinel;
            maker.seen = sentinel; maker.flags = 0; maker.bits = 0;
            int beforeIn = inputArray[0], beforeOut = output[0], beforeCalls = maker.calls;
            int beforePrivate = target == null ? 0 : target.calls;
            Throwable failure = null;
            try {
                if (target == null && mode != 14) target = new Provider();
                call(target, present, input, producer, route);
            } catch (Throwable problem) { failure = problem; }
            int trace = 0;
            if (mode != 14 && attempt == 0) trace = 1;
            boolean wrapper = mode != 1 && mode != 14;
            if (wrapper) trace = trace * 10 + 3;
            if (wrapper && present && attempt == 0) trace = trace * 10 + 9;
            boolean constructed = wrapper && present && mode != 2;
            if (constructed) trace = trace * 10 + 2;
            boolean entered = wrapper && (!present || (constructed && mode != 3));
            if (entered) trace = trace * 10 + 4;
            boolean body = entered && mode != 4;
            boolean produced = body && present && producer != null;
            if (produced) trace = trace * 10 + 5;
            boolean returned = produced && mode != 5;
            if (returned) trace = trace * 10 + 6;
            boolean mutated = returned && mode != 6 && mode != 11 && mode != 12 && mode != 15;
            boolean post = body && (!produced || (returned && mode != 6 && mode != 12 && mode != 15));
            if (post) trace = trace * 10 + 7;
            if (post && mode != 7) trace = trace * 10 + 8;
            require(Trace.events == trace);
            require(Trace.receiver == (body ? target : sentinel));
            require(target == null || target.calls == beforePrivate + (body ? 1 : 0));
            require(maker.calls == beforeCalls + (produced ? 1 : 0));
            require(Trace.seen == (returned ? maker.result : sentinel));
            require(maker.seen == (returned ? input : sentinel));
            if (returned) require(maker.flags == FLAGS && maker.bits == BITS);
            require(inputArray[0] == beforeIn + (returned && input != null ? 10 : 0) + (mutated && mode == 16 ? 1 : 0));
            require(output[0] == beforeOut + (mutated && mode != 16 ? 1 : 0));
            if (mode == 1 || mode == 2) {
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (mode >= 3 && mode <= 8) require(failure == Trace.failure);
            else if (mode == 12) require(failure instanceof ClassCastException);
            else if (mode == 14) require(failure instanceof NullPointerException);
            else if (mode == 15) require(failure instanceof ArrayIndexOutOfBoundsException);
            else require(failure == null);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
