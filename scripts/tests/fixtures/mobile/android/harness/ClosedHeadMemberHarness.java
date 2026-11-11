import fixture.*;
import java.io.IOException;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.*;
public final class ClosedHeadMemberHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned closed head member contract");
    }
    static final class ObservedMap extends HashMap<String, Object> {
        @Override public Object get(Object key) {
            Trace.gets++;
            Trace.key = key;
            Trace.events = Trace.events * 10 + 4;
            if (Trace.failAt == 4) throw Trace.runtime;
            return super.get(key);
        }
    }
    static Object call(int route, int operation, Impl factory, Class type) throws Throwable {
        if (route == 0) {
            if (operation == 0) return Core.direct(factory, type);
            if (operation == 1) return Core.concrete(factory, type);
            if (operation == 2) return Core.pick(factory, type);
            return Core.wrapped(factory, type);
        }
        if (route == 1) return ClosedHeadMemberCaller.call(operation, factory, type);
        String name = new String[]{"direct", "concrete", "pick", "wrapped"}[operation];
        Class receiver = operation == 1 ? Impl.class : Factory.class;
        if (route == 2) {
            try { return Core.class.getMethod(name, receiver, Class.class).invoke(null, factory, type); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        Class result = operation == 2 ? String.class : Map.class;
        return MethodHandles.publicLookup().findStatic(Core.class, name,
            MethodType.methodType(result, receiver, Class.class)).invoke(factory, type);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]);
        final int mode = Integer.parseInt(args[2]);
        Impl factory = mode == 1 ? null : mode == 11 ? new Impl() {
            @Override public <T> Map<String, T> query(Class<T> type) throws IOException {
                Trace.overrides++;
                return super.query(type);
            }
        } : new Impl();
        Class type = mode == 2 ? null : mode == 3 ? Integer.class : String.class;
        Trace.failAt = mode == 6 || mode == 9 ? 1 : mode == 7 ? 3
            : mode == 8 || mode == 10 ? 2 : mode == 13 ? 4 : 0;
        Trace.checked = mode >= 6 && mode <= 8 ? new IOException("owned checked failure") : null;
        Trace.error = mode == 10 ? new AssertionError("owned error") : null;
        Trace.runtime = new IllegalStateException("owned runtime failure");
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = Trace.calls = Trace.actions = Trace.gets = Trace.handled = Trace.overrides = 0;
            Trace.receiver = Trace.key = null;
            Trace.token = null;
            Trace.caught = null;
            final ObservedMap initial = new ObservedMap(), replacement = new ObservedMap();
            Object value = mode == 4 ? Integer.valueOf(23) : new String("owned value");
            initial.put("key", value);
            replacement.put("key", value);
            Trace.result = mode == 5 ? null : initial;
            Trace.action = new Action() {
                public void run() throws IOException {
                    Trace.actions++;
                    Trace.hit(3);
                    initial.put("extra", Integer.valueOf(71));
                    if (mode == 12) Trace.result = replacement;
                }
            };
            boolean entered = factory != null;
            boolean action = entered && Trace.failAt != 1;
            boolean afterAction = action && Trace.failAt != 3;
            boolean returned = afterAction && Trace.failAt != 2;
            Map expected = mode == 5 ? null : mode == 12 ? replacement : initial;
            boolean get = returned && operation == 2 && expected != null;
            int events = entered ? 1 : 0;
            if (action) events = events * 10 + 3;
            if (afterAction) events = events * 10 + 2;
            if (get) events = events * 10 + 4;
            Object result = null; Throwable failure = null;
            try { result = call(route, operation, factory, type); }
            catch (Throwable problem) { failure = problem; }
            require(Trace.events == events && Trace.calls == (entered ? 1 : 0)
                && Trace.actions == (action ? 1 : 0) && Trace.gets == (get ? 1 : 0));
            require(Trace.overrides == (mode == 11 ? 1 : 0));
            require(Trace.receiver == factory && Trace.token == (entered ? type : null));
            require(get ? "key".equals(Trace.key) : Trace.key == null);
            require(initial.containsKey("extra") == afterAction);
            boolean checked = entered && mode >= 6 && mode <= 8;
            require(Trace.handled == (checked && operation == 3 ? 1 : 0));
            require(Trace.caught == (checked && operation == 3 ? Trace.checked : null));
            if (!entered) require(failure instanceof NullPointerException);
            else if (checked) require(failure == Trace.checked);
            else if (mode == 9 || (mode == 13 && operation == 2)) require(failure == Trace.runtime);
            else if (mode == 10) require(failure == Trace.error);
            else if (operation == 2 && mode == 4) require(failure instanceof ClassCastException);
            else if (operation == 2 && mode == 5) require(failure instanceof NullPointerException);
            else require(failure == null && result == (operation == 2 ? value : expected));
            System.out.println("route=" + route + ",operation=" + operation + ",mode=" + mode
                + ",attempt=" + attempt + ",events=" + Trace.events);
        }
    }
}
