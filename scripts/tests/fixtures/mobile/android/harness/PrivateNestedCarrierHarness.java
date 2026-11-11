import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class PrivateNestedCarrierHarness {
    static void check(boolean ok) {
        if (!ok) throw new AssertionError("owned private nested carrier");
    }
    static final class Watch implements Observer {
        final Atom input, alternate;
        final Object replacement;
        final boolean chooseAlternate, fresh;
        int calls;
        Watch(Atom input, Atom alternate, Object replacement, boolean chooseAlternate, boolean fresh) {
            this.input = input; this.alternate = alternate; this.replacement = replacement; this.chooseAlternate = chooseAlternate; this.fresh = fresh;
        }
        @SuppressWarnings("unchecked")
        public void observe(Atom actual) {
            check(actual == (fresh ? Trace.created : input)); calls++; Trace.hit(4);
            if (actual != null) actual.value = replacement;
            Trace.useAlternate = chooseAlternate;
            Trace.alternate = alternate;
        }
    }
    static Atom call(int route, Atom input) throws Throwable {
        if (route == 0) return PrivateNestedCarrierCaller.call(input);
        if (route == 1) return Owner.make(input);
        if (route == 4) return Owner.create();
        Method method = Owner.class.getDeclaredMethod("keep", Atom.class);
        method.setAccessible(true);
        if (route == 2) {
            try { return (Atom)method.invoke(null, input); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        MethodHandle handle = MethodHandles.lookup().unreflect(method);
        return (Atom)handle.invokeExact(input);
    }
    static void metadata() throws Exception {
        Method method = Owner.class.getDeclaredMethod("keep", Atom.class);
        check(Modifier.isPrivate(method.getModifiers()) && Modifier.isStatic(method.getModifiers()));
        check(Owner.class.getTypeParameters().length == 1 && method.getTypeParameters().length == 1);
        TypeVariable<?> formal = method.getTypeParameters()[0], outer = Owner.class.getTypeParameters()[0];
        check(formal.getName().equals(outer.getName()) && !formal.equals(outer) && formal.getGenericDeclaration().equals(method));
        check(formal.getBounds().length == 1 && formal.getBounds()[0] == Object.class);
        ParameterizedType from = (ParameterizedType)method.getGenericParameterTypes()[0], to = (ParameterizedType)method.getGenericReturnType();
        check(from.equals(to) && from.getRawType() == Atom.class);
        ParameterizedType nested = (ParameterizedType)from.getActualTypeArguments()[0];
        Class<?> hidden = (Class<?>)nested.getRawType();
        check(hidden.getEnclosingClass() == Owner.class && Modifier.isPrivate(hidden.getModifiers()) && Modifier.isStatic(hidden.getModifiers()));
        check(nested.getActualTypeArguments().length == 1 && nested.getActualTypeArguments()[0].equals(formal));
    }
    @SuppressWarnings("unchecked")
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]); metadata();
        for (int attempt = 0; attempt < 2; attempt++) {
            Object value = mode == 1 ? null : mode == 2 ? "opaque" : mode == 3 ? new Object[1] : new Object();
            Atom input = mode == 4 ? null : new Atom(value), alternate = mode == 5 ? null : new Atom(new Object());
            Object replacement = new Object(), alternateValue = alternate == null ? null : alternate.value;
            boolean chooseAlternate = mode == 6 || mode == 5 || mode == 12;
            boolean fresh = route == 4;
            Watch watcher = new Watch(input, alternate, replacement, chooseAlternate, fresh);
            Trace.events = 0; Trace.input = Trace.result = null;
            Trace.observer = mode == 7 ? null : watcher;
            Trace.useAlternate = false; Trace.alternate = new Atom(new Object());
            Trace.allocations = 0; Trace.created = null;
            Trace.failAt = mode == 8 ? 1 : mode == 9 ? 4 : mode == 10 ? 2 : mode == 11 ? 3 : mode == 13 ? -4 : mode == 14 ? -3 : 0;
            Trace.failure = new IllegalArgumentException("owned failure"); Trace.error = new LinkageError("owned error");
            Atom actual = null; Throwable caught = null;
            try { actual = call(route, input); } catch (Throwable failure) { caught = failure; }
            boolean observed = mode != 8 && mode != 7;
            boolean observationReturned = observed && mode != 9 && mode != 13;
            boolean selected = mode != 8 && mode != 9 && mode != 13 && mode != 10;
            Atom original = fresh ? Trace.created : input;
            Atom expectedResult = chooseAlternate ? alternate : original;
            int events = mode == 8 ? 1 : mode == 9 || mode == 13 ? 14 : mode == 10 ? 142 : mode == 7 ? 123 : 1423;
            if (Trace.failAt < 0) check(caught == Trace.error);
            else if (Trace.failAt > 0) check(caught == Trace.failure);
            else check(caught == null && actual == expectedResult);
            check(Trace.events == events && watcher.calls == (observed ? 1 : 0));
            check(Trace.allocations == (fresh ? 1 : 0));
            check(Trace.input == original && Trace.result == (selected ? expectedResult : null));
            if (original != null) check(original.value == (observationReturned ? replacement : fresh ? null : value));
            if (fresh && input != null) check(input.value == value);
            if (alternate != null) check(alternate.value == alternateValue);
            if (caught != null) check(actual == null);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + events);
        }
    }
}
