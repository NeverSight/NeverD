import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.util.*;
public final class SeededScalarCarrierHarness {
    public static final class ProbeList extends AbstractList implements RandomAccess {
        final Object[] contents;
        int reads, sizes;
        long indices;
        ProbeList(Object... contents) { this.contents = contents; }
        public int size() { sizes++; return contents.length; }
        public Object get(int index) { reads++; indices = indices * 10 + index + 1; return contents[index]; }
    }
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("original seeded scalar carrier contract");
    }
    static int call(int entry, int route, List values, Object needle) throws Throwable {
        if (route == 0) {
            if (entry == 0) return Core.create(values, needle);
            if (entry == 1) return Core.guarded(values, needle);
            return Core.locked(values, needle);
        }
        if (route == 1) return SeededScalarCarrierCaller.run(entry, values, needle);
        String name = entry == 0 ? "create" : entry == 1 ? "guarded" : "locked";
        if (route == 2) {
            try { return (Integer) Core.class.getMethod(name, List.class, Object.class).invoke(null, values, needle); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        return (int) MethodHandles.lookup().findStatic(Core.class, name,
            MethodType.methodType(int.class, List.class, Object.class)).invokeExact(values, needle);
    }
    public static void main(String[] args) throws Throwable {
        int entry = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        RuntimeException original = new IllegalArgumentException("owned seeded scalar failure");
        Trace.failure = original;
        Trace.failAt = mode == 4 ? 1 : mode == 5 ? 3 : mode == 6 ? 2 : mode == 7 ? 4 : 0;
        long events = 0;
        Object held = Trace.lock;
        for (int attempt = 0; attempt < 2; attempt++) {
            ProbeList values = mode == 1 ? null : mode == 8 ? new ProbeList() : mode == 9
                ? new ProbeList(new Object()) : new ProbeList(new Item(2), new Item(7), new Item(13), new Item(21));
            int[] key = {13, 0};
            Object needle = mode == 2 ? null : mode == 3 ? "wrong original key" : key;
            Trace.last = null; Trace.observedResult = Trace.result = -777;
            if (attempt == 0) events = 4;
            if (mode != 7) {
                events = events * 10 + 1;
                if (mode != 1 && mode != 4 && mode != 8 && mode != 9) {
                    events = events * 10 + 3;
                    if (mode == 0 || mode == 6) events = events * 10 + 3;
                }
                if (mode == 0 || mode == 6 || mode == 8) events = events * 10 + 2;
            }
            Throwable error = null;
            int result = -999;
            try { result = call(entry, route, values, needle); }
            catch (Throwable caught) { error = caught; }
            if (entry == 1) { require(error == null); error = Trace.last; }
            if (mode == 0 || mode == 8) { require(error == null); require(result == (mode == 8 ? -1 : 2)); }
            else {
                require(error != null && result == -999);
                if (mode == 4 || mode == 5 || mode == 6) require(error == original);
                else if (mode == 1 || mode == 2) require(error.getClass() == NullPointerException.class);
                else if (mode == 3 || mode == 9) require(error.getClass() == ClassCastException.class);
                else if (attempt == 0) require(error.getClass() == ExceptionInInitializerError.class && error.getCause() == original);
                else require(error.getClass() == NoClassDefFoundError.class);
            }
            require(Trace.events == events && Trace.lock == held && !Thread.holdsLock(held));
            require(Trace.held == (entry == 2));
            if (mode != 7) require(Trace.values == values && Trace.needle == needle);
            require(key[1] == (mode == 0 || mode == 6 ? 2 : 0));
            if (values != null) {
                int reads = mode == 0 || mode == 6 ? 2 : mode == 2 || mode == 3 || mode == 5 || mode == 9 ? 1 : 0;
                long indices = reads == 2 ? 23 : mode == 9 ? 1 : reads == 1 ? 2 : 0;
                require(values.reads == reads && values.indices == indices);
                require(values.sizes == (mode == 4 || mode == 7 ? 0 : 1));
            }
            require(Trace.observedResult == (mode == 0 || mode == 6 ? 2 : mode == 8 ? -1 : -777));
            System.out.println("entry=" + entry + ",route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
