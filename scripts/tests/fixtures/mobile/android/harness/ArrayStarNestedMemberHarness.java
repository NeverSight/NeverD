import fixture.*;
import java.io.IOException;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.*;
public final class ArrayStarNestedMemberHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned array star nested member contract");
    }
    static final class ObservedSet extends HashSet<Object> {
        @Override public Iterator<Object> iterator() { Trace.inspected++; return super.iterator(); }
        @Override public int size() { Trace.inspected++; return super.size(); }
        @Override public Object[] toArray() { Trace.inspected++; return super.toArray(); }
    }
    static Object call(int route, int operation, Impl factory, Object value, String context, Class<?>[] types) throws Throwable {
        if (route == 0) {
            if (operation == 0) return Core.direct(factory, value, types);
            if (operation == 1) return Core.concrete(factory, value, types);
            if (operation == 2) return Core.wrapped(factory, value, types);
            if (operation == 3) return Core.named(factory, value, context, types);
            if (operation == 4) return Core.routed(factory, value, types);
            return Core.routedNamed(factory, value, context, types);
        }
        if (route == 1) return ArrayStarNestedMemberCaller.call(operation, factory, value, context, types);
        String name = new String[]{"direct", "concrete", "wrapped", "named", "routed", "routedNamed"}[operation];
        Class receiver = operation == 1 ? Impl.class : Factory.class;
        Class resultType = operation >= 4 ? Result.class : Set.class;
        if (route == 2) {
            try {
                if (operation == 3 || operation == 5) return Core.class.getMethod(name, receiver, Object.class, String.class, Class[].class).invoke(null, factory, value, context, types);
                return Core.class.getMethod(name, receiver, Object.class, Class[].class).invoke(null, factory, value, types);
            }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (operation == 3 || operation == 5) return MethodHandles.publicLookup().findStatic(Core.class, name,
            MethodType.methodType(resultType, receiver, Object.class, String.class, Class[].class)).asFixedArity().invoke(factory, value, context, types);
        return MethodHandles.publicLookup().findStatic(Core.class, name,
            MethodType.methodType(resultType, receiver, Object.class, Class[].class)).asFixedArity().invoke(factory, value, types);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]);
        final int mode = Integer.parseInt(args[2]);
        Impl factory = mode == 1 ? null : mode == 11 ? new Impl() {
            @Override public <T> Set<Box<T>> query(T value, Class<?>... types) throws IOException {
                Trace.overrides++;
                return super.query(value, types);
            }
        } : new Impl();
        final Object value = mode == 2 ? null : mode == 3 ? Integer.valueOf(23) : new Object();
        final String context = mode == 15 ? null : new String("owned context");
        Trace.failAt = mode == 6 || mode == 9 ? 1 : mode == 7 ? 3
            : mode == 8 || mode == 10 ? 2 : mode == 13 ? 4 : 0;
        Trace.checked = (mode >= 6 && mode <= 8) || mode == 13 ? new IOException("owned checked failure") : null;
        Trace.error = mode == 10 ? new AssertionError("owned error") : null;
        Trace.runtime = new IllegalStateException("owned runtime failure");
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = Trace.calls = Trace.actions = Trace.inspected = Trace.handled = Trace.overrides = Trace.collections = 0;
            Trace.receiver = Trace.value = null;
            Trace.types = null;
            Trace.context = null;
            Trace.caught = null;
            Trace.collection = null;
            Entry.factory = factory;
            final Class<?>[] types = mode == 4 ? null : mode == 14 ? new Class<?>[0]
                : new Class<?>[]{mode == 15 ? null : String.class, int[].class};
            final ObservedSet initial = new ObservedSet(), replacement = new ObservedSet();
            initial.add(Integer.valueOf(17)); replacement.add(new Object());
            Trace.result = mode == 5 ? null : initial;
            Trace.action = new Action() {
                public void run() throws IOException {
                    Trace.actions++;
                    Trace.hit(3);
                    if (types != null && types.length != 0) types[0] = Integer.class;
                    if (mode == 12) Trace.result = replacement;
                }
            };
            boolean entered = factory != null;
            boolean action = entered && Trace.failAt != 1;
            boolean afterAction = action && Trace.failAt != 3;
            boolean returned = afterAction && Trace.failAt != 2;
            int events = entered ? 1 : 0;
            if (action) events = events * 10 + 3;
            if (afterAction) events = events * 10 + 2;
            if (operation == 2) events = events * 10 + 4;
            boolean collected = returned && operation >= 4;
            if (collected) events = events * 10 + 5;
            Object expected = mode == 5 ? null : mode == 12 ? replacement : initial;
            Object result = null; Throwable failure = null;
            try { result = call(route, operation, factory, value, context, types); }
            catch (Throwable problem) { failure = problem; }
            require(Trace.events == events && Trace.calls == (entered ? 1 : 0)
                && Trace.actions == (action ? 1 : 0) && Trace.inspected == 0);
            require(Trace.context == (entered && (operation == 3 || operation == 5) ? context : null));
            require(Trace.collections == (collected ? 1 : 0) && Trace.collection == (collected ? expected : null));
            require(Trace.overrides == (mode == 11 ? 1 : 0));
            require(Trace.receiver == factory && Trace.value == (entered ? value : null)
                && Trace.types == (entered ? types : null));
            if (types != null && types.length != 0) require(types[0] == (afterAction ? Integer.class : mode == 15 ? null : String.class));
            boolean caught = entered && mode >= 6 && mode <= 8 && operation == 2;
            require(Trace.handled == (caught ? 1 : 0) && Trace.caught == (caught ? Trace.checked : null));
            if (!entered) require(failure instanceof NullPointerException);
            else if ((mode >= 6 && mode <= 8) || (mode == 13 && operation == 2)) require(failure == Trace.checked);
            else if (mode == 9) require(failure == Trace.runtime);
            else if (mode == 10) require(failure == Trace.error);
            else {
                require(returned && failure == null);
                Object opaque = result;
                if (operation >= 4) { require(result instanceof Result); opaque = ((Result)result).values; }
                require(opaque == expected);
                if (opaque != null) {
                    Object marker = new Object();
                    ((Set)opaque).add(marker);
                    require(((Set)expected).contains(marker));
                }
            }
            System.out.println("route=" + route + ",operation=" + operation + ",mode=" + mode
                + ",attempt=" + attempt + ",events=" + Trace.events);
        }
    }
}
