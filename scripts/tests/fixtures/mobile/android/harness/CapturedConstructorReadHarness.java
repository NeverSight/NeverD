import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public class CapturedConstructorReadHarness {
    static void check(boolean value) {
        if (!value) throw new AssertionError("captured constructor read contract");
    }
    static Base make(int route, Owner owner, Object payload, boolean wide) throws Throwable {
        if (route == 0) return Client.make(owner, payload, wide);
        if (route == 1) return CapturedConstructorReadCaller.make(owner, payload, wide);
        if (route == 2) {
            try {
                return (wide ? Owner.WideWorker.class : Owner.Worker.class)
                        .getConstructor(Owner.class, Object.class).newInstance(owner, payload);
            } catch (InvocationTargetException error) { throw error.getCause(); }
        }
        if (wide) {
            MethodHandle ctor = MethodHandles.lookup().findConstructor(Owner.WideWorker.class,
                    MethodType.methodType(void.class, Owner.class, Object.class));
            return (Owner.WideWorker) ctor.invokeExact(owner, payload);
        }
        MethodHandle ctor = MethodHandles.lookup().findConstructor(Owner.Worker.class,
                MethodType.methodType(void.class, Owner.class, Object.class));
        return (Owner.Worker) ctor.invokeExact(owner, payload);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        boolean wide = Integer.parseInt(args[2]) != 0;
        Trace.initialWord = 37; Trace.initialWide = 0xfedcba9876543210L;
        Trace.failure = new IllegalStateException("owned captured constructor failure");
        Trace.failAt = mode <= 4 ? mode : 0;
        if (mode == 6) {
            check(Provider.word == 37 && Trace.events == 1);
            Provider.word = 61; Provider.wide = 0x76543210fedcba98L;
        }
        Owner owner = new Owner(), other = new Owner();
        Object payload = mode == 5 ? null : new Object();
        Base previous = null;
        for (int attempt = 0; attempt < 2; attempt++) {
            Owner captured = attempt == 0 ? owner : other;
            Trace.events = 0; Trace.observed = null; Trace.captured = null;
            Trace.nextWord = 101 + attempt; Trace.nextWide = 0x8123456789abcdefL + attempt;
            Base value = null; Throwable failure = null;
            try { value = make(route, captured, payload, wide); } catch (Throwable error) { failure = error; }
            int events = attempt == 0 && mode != 6 ? 1 : 0;
            if (mode != 1) {
                if (attempt == 0) events = events * 10 + 2;
                if (mode != 2) events = events * 10 + 3;
                if (mode == 0 || mode == 4 || mode == 6) events = events * 10 + 4;
            }
            if (mode == 1 && attempt == 1) events = 0;
            check(Trace.events == events && Trace.baseCalls == (mode == 1 ? 0 : attempt + 1));
            if (mode == 1 || mode == 2) {
                check(value == null);
                if (attempt == 0) check(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
                else check(failure instanceof NoClassDefFoundError);
            } else if (mode == 0 || mode == 6) {
                check(failure == null && value != null && value == Trace.observed && value != previous);
                check((wide ? ((Owner.WideWorker) value).owner() : ((Owner.Worker) value).owner()) == captured);
                previous = value;
            } else check(value == null && failure == Trace.failure);
            if (mode == 1) check(Trace.observed == null && Trace.captured == null);
            else {
                Base observed = Trace.observed;
                int expectedWord = attempt == 0 ? (mode == 6 ? 61 : 37) : 101;
                long expectedWide = attempt == 0 ? (mode == 6 ? 0x76543210fedcba98L : 0xfedcba9876543210L) : 0x8123456789abcdefL;
                check(observed != null && observed.payload == payload && observed.spare == null);
                check(Trace.captured == captured && observed.owner() == captured);
                check(observed.word == (wide ? 0 : expectedWord) && observed.wide == (wide ? expectedWide : 0));
            }
            System.out.println("route=" + route + ",mode=" + mode + ",wide=" + (wide ? 1 : 0)
                    + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
