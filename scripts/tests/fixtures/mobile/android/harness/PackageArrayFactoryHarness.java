import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.*;
public final class PackageArrayFactoryHarness {
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned package array factory");
    }
    @SuppressWarnings("unchecked")
    static <E extends Throwable> void raise(Throwable value) throws E { throw (E) value; }
    static final class View extends AbstractList<Object> {
        final Object[] values; final boolean fail;
        View(Object[] values, boolean fail) { this.values = values; this.fail = fail; }
        public int size() { return values.length; }
        public Object get(int index) {
            Trace.reads++; Trace.hit(6);
            if (fail) throw Trace.failure;
            return values[index];
        }
        public Object set(int index, Object value) {
            Object previous = values[index]; values[index] = value; return previous;
        }
    }
    static Object call(int route, int operation, Object[] values) throws Throwable {
        if (route == 0) {
            if (operation == 0) return Core.make(values);
            if (operation == 1) return Core.narrowed(values);
            Core.ignored(values); return null;
        }
        if (route == 1) {
            if (operation == 0) return PackageArrayFactoryCaller.make(values);
            if (operation == 1) return PackageArrayFactoryCaller.narrowed(values);
            PackageArrayFactoryCaller.ignored(values); return null;
        }
        String name = operation == 0 ? "make" : operation == 1 ? "narrowed" : "ignored";
        if (route == 2) {
            try { return Core.class.getMethod(name, Object[].class).invoke(null, (Object) values); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        Class<?> result = operation == 0 ? List.class : operation == 1 ? String.class : void.class;
        return MethodHandles.publicLookup().findStatic(Core.class, name,
            MethodType.methodType(result, Object[].class)).invokeWithArguments(Collections.singletonList(values));
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Class<?> provider = Class.forName("fixture.Provider");
        require(!Modifier.isPublic(provider.getModifiers()));
        require(provider.getDeclaredMethod("make", Object[].class).getModifiers() == Modifier.STATIC);
        for (int attempt = 0; attempt < 2; attempt++) for (int operation = 0; operation < 3; operation++) {
            final Object[] values = mode == 2 ? null : mode == 1 || mode == 6
                ? new String[]{"owned-a", "owned-b"} : new Object[]{"owned-a", "owned-b"};
            final View initial = new View(mode == 10 ? new Integer[]{23} : values == null
                ? new Object[]{"owned-null-input"} : values, mode == 16);
            final View alternate = new View(new Object[]{"owned-alternate"}, false);
            final Exception checked = new Exception("owned checked identity");
            final Object replacement = new Object();
            Trace.events = Trace.calls = Trace.actions = Trace.reads = 0;
            Trace.input = null; Trace.index = mode == 17 ? 9 : 0;
            Trace.result = mode == 8 ? replacement : mode == 9 ? null : initial;
            Trace.failAt = mode == 11 ? 1 : mode == 12 ? 2 : mode == 13 ? 3 : mode == 14 ? 4 : mode == 19 ? 5 : 0;
            Trace.failure = new RuntimeException("owned exact failure"); Trace.error = new AssertionError("owned exact error");
            final int scenario = mode;
            Trace.action = mode == 15 ? null : new Runnable() { public void run() {
                require(Trace.input == values && Trace.calls == 1 && Trace.events == 31);
                Trace.actions++; Trace.hit(5);
                if (scenario == 3) throw Trace.failure;
                if (scenario == 4) throw Trace.error;
                if (scenario == 5) PackageArrayFactoryHarness.<RuntimeException>raise(checked);
                if (scenario == 6) values[0] = replacement;
                if (scenario == 7) Trace.result = alternate;
                if (scenario == 18) values[0] = "owned-mutated";
            }};
            Object answer = null; Throwable failure = null;
            try { answer = call(route, operation, values); } catch (Throwable caught) { failure = caught; }
            if (mode == 13 || mode == 11) {
                require(failure == Trace.failure && Trace.events == (mode == 13 ? 3 : 31)
                    && Trace.calls == 0 && Trace.actions == 0 && Trace.input == null); continue;
            }
            require(Trace.calls == 1 && Trace.input == values);
            if (mode == 15) { require(failure instanceof NullPointerException && Trace.events == 31 && Trace.actions == 0); continue; }
            require(Trace.actions == 1);
            if (mode == 3 || mode == 4 || mode == 5 || mode == 19) {
                require(failure == (mode == 4 ? Trace.error : mode == 5 ? checked : Trace.failure) && Trace.events == 315); continue;
            }
            if (mode == 6) { require(failure instanceof ArrayStoreException && Trace.events == 315 && "owned-a".equals(values[0])); continue; }
            if (mode == 8) { require(failure instanceof ClassCastException && Trace.events == 315); continue; }
            if (mode == 12 || mode == 14) {
                require(failure == Trace.failure && Trace.events == (mode == 12 ? 3152 : 31524) && Trace.reads == 0); continue;
            }
            View chosen = mode == 9 ? null : mode == 7 ? alternate : initial;
            require(Trace.result == chosen);
            if (operation == 1) {
                if (chosen == null) { require(failure instanceof NullPointerException && Trace.events == 31524 && Trace.reads == 0); continue; }
                require(Trace.events == 315246 && Trace.reads == 1);
                if (mode == 10) require(failure instanceof ClassCastException);
                else if (mode == 16) require(failure == Trace.failure);
                else if (mode == 17) require(failure instanceof IndexOutOfBoundsException);
                else require(failure == null && answer == chosen.values[0]);
            } else {
                require(failure == null && Trace.events == 31524 && Trace.reads == 0 && answer == (operation == 0 ? chosen : null));
                if (operation == 0 && (mode == 0 || mode == 1 || mode == 18)) {
                    @SuppressWarnings("unchecked") List<Object> list = (List<Object>) answer;
                    Object previous = values[0]; require(list.set(0, "owned-written") == previous && values[0] == "owned-written");
                    if (mode == 1) {
                        try { list.set(0, replacement); throw new AssertionError("owned package array factory"); }
                        catch (ArrayStoreException expected) { require(values[0] == "owned-written"); }
                    }
                }
            }
        }
        System.out.println("attempts=2,operations=3,package-array-factory=matched");
    }
}
