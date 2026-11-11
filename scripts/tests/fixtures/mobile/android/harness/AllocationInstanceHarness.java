import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class AllocationInstanceHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("allocation instance contract"); }
    static Payload call(Provider provider, Object input, int variant, int route) throws Throwable {
        if (route == 0) return variant == 0 ? Core.make(provider, input) : provider.self(input);
        if (route == 1) return variant == 0 ? AllocationInstanceCaller.make(provider, input) : AllocationInstanceCaller.self(provider, input);
        if (route == 2) {
            try { return variant == 0 ? (Payload) Core.class.getMethod("make", Provider.class, Object.class).invoke(null, provider, input)
                                      : (Payload) Provider.class.getMethod("self", Object.class).invoke(provider, input); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        MethodType type = variant == 0 ? MethodType.methodType(Payload.class, Provider.class, Object.class)
                                       : MethodType.methodType(Payload.class, Object.class);
        MethodHandle handle = variant == 0 ? MethodHandles.publicLookup().findStatic(Core.class, "make", type)
                                           : MethodHandles.publicLookup().findVirtual(Provider.class, "self", type);
        return (Payload) handle.invokeExact(provider, input);
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]);
        int child = Integer.parseInt(args[2]), mode = Integer.parseInt(args[3]);
        int event = child == 0 ? 2 : 3;
        Provider receiver = mode == 4 ? null : child == 0 ? new Provider() : new Derived();
        Trace.failAt = mode == 1 ? 1 : mode == 2 ? event : mode == 3 ? 4 : 0;
        Trace.failure = new IllegalArgumentException("owned instance failure");
        int[] input = {7}, result = {11}, alternate = {19};
        Trace.result = result; Trace.alternate = alternate;
        Object argument = mode == 5 ? null : input;
        Payload previous = null;
        for (int attempt = 0; attempt < 2; attempt++) {
            Object sentinel = new Object(); Trace.seen = sentinel; Trace.events = 0;
            int reads = Trace.reads, oldInput = input[0];
            Payload actual = null; Throwable failure = null;
            try { actual = call(receiver, argument, variant, route); } catch (Throwable caught) { failure = caught; }
            boolean entered = mode != 1 && mode != 4;
            int expected = (variant == 1 && mode == 4) ? 0 : attempt == 0 ? 1 : 0;
            if (entered) { expected = expected * 10 + event; if (mode != 2) expected = expected * 10 + 4; }
            require(Trace.events == expected && Trace.reads == reads + (entered ? 1 : 0));
            require(Trace.seen == (entered ? argument : sentinel));
            require(input[0] == oldInput + (entered && mode != 2 && mode != 5 ? 1 : 0));
            if (mode == 1) {
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (mode == 2 || mode == 3) require(failure == Trace.failure);
            else if (mode == 4) require(failure instanceof NullPointerException);
            else {
                Object wanted = child == 0 ? result : alternate;
                require(failure == null && actual != null && actual != previous && actual.receiver == receiver && actual.value == wanted);
                ((int[]) wanted)[0]++;
                require(((int[]) actual.value)[0] == ((int[]) wanted)[0]);
                previous = actual;
            }
            if (failure != null) require(actual == null);
            System.out.println("variant=" + variant + ",route=" + route + ",child=" + child + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
