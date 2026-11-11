import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class PairedVoidHarness {
    static final int FLAGS = Integer.MIN_VALUE + 61;
    static final long BITS = 0xfff8abcd12345678L;
    static void require(boolean value) { if (!value) throw new AssertionError("paired void invocation changed"); }
    static final class Maker implements Pair<int[], Object> {
        Object result, seen, token; int calls, flags; long bits;
        public Object apply(int[] input, int flag, Object key, long raw) {
            calls++; Trace.hit(4); seen = input; flags = flag; token = key; bits = raw;
            if (input != null) input[0] += 10;
            return result;
        }
    }
    static void call(Provider target, Pair pair, Object input, Object token, int route) throws Throwable {
        if (route == 0) { Client.call(target, FLAGS, pair, input, token, BITS); return; }
        if (route == 1) { PairedVoidCaller.call(target, FLAGS, pair, input, token, BITS); return; }
        if (route == 2) {
            try { Client.class.getMethod("call", Provider.class, int.class, Pair.class, Object.class, Object.class, long.class)
                    .invoke(null, target, FLAGS, pair, input, token, BITS); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
            return;
        }
        MethodHandles.publicLookup().findStatic(Client.class, "call",
                MethodType.methodType(void.class, Provider.class, int.class, Pair.class, Object.class, Object.class, long.class))
                .invokeExact(target, FLAGS, pair, input, token, BITS);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), kind = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        for (Class<?> owner : new Class<?>[]{Provider.class, Primary.class, Alternate.class}) {
            Method method = owner.getDeclaredMethod("accept", int.class, Pair.class, Object.class, Object.class, long.class);
            TypeVariable<?>[] formals = method.getTypeParameters();
            require(formals.length == 2 && formals[0].getBounds()[0] == Object.class && formals[1].getBounds()[0] == Object.class);
            Type[] slots = ((ParameterizedType) method.getGenericParameterTypes()[1]).getActualTypeArguments();
            require(slots[0].equals(formals[0]) && slots[1].equals(formals[1]));
            require(method.getGenericParameterTypes()[2].equals(formals[0]) && method.getReturnType() == void.class);
            require(Modifier.isAbstract(method.getModifiers()) == (owner == Provider.class));
        }
        int entry = kind == 0 ? 3 : 9, delta = kind == 0 ? 1 : 3;
        Trace.failAt = mode == 1 ? 1 : mode == 2 ? 2 : mode == 3 ? entry : mode == 4 ? 4 : mode == 5 ? 5 : 0;
        Trace.failure = new RuntimeException("owned paired void failure");
        Provider target = null;
        Maker maker = new Maker();
        int[] inputArray = {11}, output = {31};
        Object input = mode == 7 ? null : mode == 9 ? new Object() : inputArray;
        maker.result = mode == 8 ? null : mode == 10 ? new Object() : mode == 12 ? input : mode == 13 ? new int[0] : output;
        Pair pair = mode == 6 ? null : maker;
        Object sentinel = new Object(), token = new Object();
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.result = sentinel; Trace.receiver = sentinel;
            maker.seen = sentinel; maker.token = sentinel; maker.flags = 0; maker.bits = 0;
            int beforeIn = inputArray[0], beforeOut = output[0], beforeCalls = maker.calls;
            int beforeProvider = target == null ? 0 : target.calls;
            Throwable failure = null;
            try {
                if (target == null && mode != 11) target = kind == 0 ? new Primary() : new Alternate();
                call(target, pair, input, token, route);
            } catch (Throwable problem) { failure = problem; }
            int trace = mode != 11 && attempt == 0 ? 1 : 0;
            if (mode != 11 && mode != 1 && (attempt == 0 || mode == 2)) trace = trace * 10 + 2;
            boolean entered = mode != 1 && mode != 2 && mode != 11;
            if (entered) trace = trace * 10 + entry;
            boolean body = entered && mode != 3;
            boolean produced = body && mode != 6 && mode != 9;
            if (produced) trace = trace * 10 + 4;
            boolean returned = produced && mode != 4;
            if (returned) trace = trace * 10 + 5;
            boolean post = returned && mode != 5 && mode != 10 && mode != 13;
            if (post) trace = (trace * 10 + 6) * 10 + 7;
            boolean mutated = post && mode != 8;
            require(Trace.events == trace && Trace.receiver == (body ? target : sentinel));
            require(target == null || target.calls == beforeProvider + (body ? 1 : 0));
            require(maker.calls == beforeCalls + (produced ? 1 : 0));
            require(Trace.result == (returned ? maker.result : sentinel));
            require(maker.seen == (returned ? input : sentinel));
            if (returned) require(maker.flags == FLAGS && maker.bits == BITS && maker.token == token);
            require(inputArray[0] == beforeIn + (returned && input != null ? 10 : 0) + (mutated && mode == 12 ? delta : 0));
            require(output[0] == beforeOut + (mutated && mode != 12 ? delta : 0));
            if (mode == 1) {
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (mode >= 2 && mode <= 5) require(failure == Trace.failure);
            else if (mode == 6 || mode == 11) require(failure instanceof NullPointerException);
            else if (mode == 9 || mode == 10) require(failure instanceof ClassCastException);
            else if (mode == 13) require(failure instanceof ArrayIndexOutOfBoundsException);
            else require(failure == null);
            System.out.println("route=" + route + ",kind=" + kind + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
