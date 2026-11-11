import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ConditionalScalarHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("conditional scalar conversion contract");
    }
    static Target call(Creator creator, Object seed, int route) throws Throwable {
        if (route == 0) return creator.make(seed);
        if (route == 1) return ConditionalScalarCaller.make(creator, seed);
        if (route == 2) {
            try { return (Target) Creator.class.getMethod("make", Object.class).invoke(creator, seed); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        return (Target) MethodHandles.publicLookup().findVirtual(Creator.class, "make",
                MethodType.methodType(Target.class, Object.class)).invokeExact(creator, seed);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        int[] bits = {0x40600000, 0xc0600000, 0x80000000, 0x7fc01234, 0x7f800000,
                0xff800000, 0x4f000000, 0xcf000000, 0x3f800001, 0xbf800001};
        int selected = mode >= 6 && mode < 16 ? mode - 6 : 0;
        Object seed = mode >= 16 ? null : new Object();
        Trace.failure = new IllegalStateException("owned conditional conversion failure");
        Trace.failAt = mode >= 1 && mode <= 5 ? mode : 0;
        Creator creator = new Creator();
        Target previous = null;
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.seen = null;
            Trace.value = Float.intBitsToFloat(bits[selected]);
            Trace.tail = 7 + attempt;
            float original = Trace.value;
            int reads = Trace.reads;
            Target result = null; Throwable failure = null;
            try { result = call(creator, seed, route); }
            catch (Throwable error) { failure = error; }
            String events = seed == null ? "245" : "2345";
            if (attempt == 0) events = "1" + events;
            else if (mode != 1 && mode != 2) events = events.substring(1);
            if (mode == 1 && attempt == 1) events = "";
            if (mode == 2 && attempt == 1) events = "";
            if (Trace.failAt != 0 && events.indexOf((char) ('0' + Trace.failAt)) >= 0)
                events = events.substring(0, events.indexOf((char) ('0' + Trace.failAt)) + 1);
            require(Trace.events == (events.isEmpty() ? 0 : Long.parseLong(events)));
            if (mode == 1 || mode == 2) {
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (mode >= 3 && mode <= 5) require(failure == Trace.failure);
            else {
                require(failure == null && result != null && result != previous);
                require(result.seed == seed && result.converted == (seed == null ? 0 : (int) Math.ceil((double) original)));
                require(result.tail == 7 + attempt);
                previous = result;
            }
            boolean entered = seed != null && mode != 1 && mode != 2;
            require(Trace.reads == reads + (entered ? 1 : 0));
            require(Trace.seen == (entered ? seed : null));
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
