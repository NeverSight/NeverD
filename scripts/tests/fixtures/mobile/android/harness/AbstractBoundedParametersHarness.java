import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.*;

public final class AbstractBoundedParametersHarness {
    private static int values, identities, states, failures;
    private interface Call { void run(Core<String> r, String m, int i, Object b, long w, boolean e) throws Throwable; }
    private static final class Overriding extends Core<String> {
        public void read(String m, int i, Box<String> b, long w, boolean e) {
            calls += 3; super.read(m, i, b, w, e);
        }
    }
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("bounded parameter identity changed");
        identities++;
    }
    private static Method entry(Class<?> owner, Class<?> parameter) throws Exception {
        Method m = owner.getDeclaredMethod("read", String.class, int.class, parameter, long.class, boolean.class);
        m.setAccessible(true); return m;
    }
    private static void invoke(Method m, Core<String> r, Object... args) throws Throwable {
        try { m.invoke(r, args); } catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    private static void state(Core<String> r, int before, String m, int i, Object b, long w, boolean e) {
        if (r.calls != before + (r instanceof Overriding ? 4 : 1) || r.index != i || r.wide != w || r.enabled != e)
            throw new AssertionError("bounded parameter effects changed");
        same(r.marker, m); same(r.seen, b); states++;
    }
    public static void main(String[] args) throws Throwable {
        boolean rebuilt = args.length == 1 && args[0].equals("rebuilt");
        if (Middle.class.getSuperclass() != Parent.class || Parent.class.getSuperclass() != Root.class
                || Core.class.getSuperclass() != Middle.class || !Modifier.isAbstract(Middle.class.getModifiers()))
            throw new AssertionError("abstract parent chain changed");
        TypeVariable<?>[] variables = Middle.class.getTypeParameters();
        if (variables.length != 2 || variables[1].getGenericDeclaration() != Middle.class
                || !(((ParameterizedType) variables[1].getBounds()[0]).getActualTypeArguments()[0].equals(variables[0]))
                || !entry(Middle.class, Container.class).getGenericParameterTypes()[2].equals(variables[1]))
            throw new AssertionError("bounded class-formal ownership changed");
        int additions = 0;
        for (Method m : Middle.class.getDeclaredMethods()) if (m.isBridge()) {
            if (!m.isSynthetic() || Modifier.isAbstract(m.getModifiers()) || !Modifier.isProtected(m.getModifiers())
                    || m.getParameterTypes()[2] != Object.class)
                throw new AssertionError("compiler bridge role changed");
            additions++;
        }
        if (additions != (rebuilt ? 1 : 0)) throw new AssertionError("compiler addition inventory changed");
        List<Call> routes = new ArrayList<>();
        List<Call> erased = new ArrayList<>();
        for (Class<?> owner : new Class<?>[]{Root.class, Middle.class, Core.class}) {
            Class<?>[] parameters = owner == Root.class ? new Class<?>[]{Object.class}
                : owner == Middle.class ? new Class<?>[]{Container.class}
                : new Class<?>[]{Object.class, Container.class, Box.class};
            for (Class<?> parameter : parameters) {
                Method m = entry(owner, parameter); MethodHandle mh = MethodHandles.lookup().unreflect(m);
                Call reflection = (r, a, i, b, w, e) -> invoke(m, r, a, i, b, w, e);
                Call handle = (r, a, i, b, w, e) -> mh.invoke(r, a, i, b, w, e);
                routes.add(reflection); routes.add(handle);
                if (parameter == Object.class) { erased.add(reflection); erased.add(handle); }
            }
        }
        routes.add((r, m, i, b, w, e) -> r.typed(m, i, (Box<String>) b, w, e));
        routes.add((r, m, i, b, w, e) -> r.dispatch(m, i, (Box<String>) b, w, e));
        Core<String>[] receivers = new Core[]{new Core<String>(), new Overriding()};
        String[] markers = {null, "owned"};
        int[] indices = {Integer.MIN_VALUE, 0, Integer.MAX_VALUE};
        long[] wides = {Long.MIN_VALUE, 0, Long.MAX_VALUE};
        for (Core<String> r : receivers) for (String m : markers) for (int i : indices)
            for (long w : wides) for (boolean e : new boolean[]{false, true}) for (Call call : routes) {
                Box<String> b = new Box<>(); r.value = m;
                int before = r.calls; call.run(r, m, i, b, w, e);
                state(r, before, m, i, b, w, e); same(b.value, m);
                if (b.index != i || b.wide != w || b.enabled != e) throw new AssertionError("wide arguments changed");
                values++;
            }
        RuntimeException problem = new IllegalStateException("owned callback failure");
        for (Core<String> r : receivers) {
            r.failure = problem;
            for (Call call : routes) for (Object b : new Object[]{null, new Box<String>()}) {
                int before = r.calls;
                try { call.run(r, "failure", -1, b, Long.MIN_VALUE, true); throw new AssertionError("failure missing"); }
                catch (RuntimeException actual) { same(actual, problem); failures++; }
                state(r, before, "failure", -1, b, Long.MIN_VALUE, true);
                if (b != null) same(((Box<?>) b).value, null);
            }
            for (Call call : erased) for (Object invalid : new Object[]{new Object(), new Other<String>()}) {
                int before = r.calls;
                try { call.run(r, null, 1, invalid, 3, false); throw new AssertionError("cast missing"); }
                catch (ClassCastException expected) { failures++; }
                if (r.calls != before) throw new AssertionError("cast happened after target");
                states++;
            }
            r.failure = null;
            for (Call call : routes) {
                int before = r.calls;
                try { call.run(r, null, 1, null, 3, false); throw new AssertionError("null box accepted"); }
                catch (NullPointerException expected) { failures++; }
                state(r, before, null, 1, null, 3, false);
            }
        }
        for (Call call : routes) {
            try { call.run(null, null, 0, null, 0, false); throw new AssertionError("null receiver accepted"); }
            catch (NullPointerException expected) { failures++; }
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("abstract-bounded-declarations-and-concrete-dispatch=retained,compiler-slot=reported");
    }
}
