import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;

public class GuardedScalarParametersHarness {
    static int values, identities, states, failures;
    static final String[] names = {"contains", "matches"};
    static void check(boolean yes) { values++; if (!yes) throw new AssertionError("value " + values); }
    static void same(Object a, Object b) { identities++; if (a != b) throw new AssertionError("identity " + identities); }
    static void state(boolean yes) { states++; if (!yes) throw new AssertionError("state " + states); }
    static boolean invoke(int route, int kind, Core core, Object input) throws Throwable {
        if (route <= 2) {
            if (route == 0) return kind == 0 ? core.contains(input) : core.matches(input);
            Api<?> api = core; if (route == 2) api = (Api) core;
            return kind == 0 ? api.contains(input) : api.matches(input);
        }
        try {
            if (route != 5) return ((Boolean) (route == 3 ? Core.class : Api.class).getMethod(names[kind], Object.class).invoke(core, input)).booleanValue();
            return (boolean) MethodHandles.publicLookup().findVirtual(Api.class, names[kind], MethodType.methodType(boolean.class, Object.class)).invoke(core, input);
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    static int boxCalls(Object box) { return box instanceof LongBox ? ((LongBox) box).calls : ((DoubleBox) box).calls; }
    static int boxObserved(Object box) { return box instanceof LongBox ? ((LongBox) box).observed : ((DoubleBox) box).observed; }
    static Object boxMarker(Object box) { return box instanceof LongBox ? ((LongBox) box).marker : ((DoubleBox) box).marker; }
    static void boxFailure(Object box, RuntimeException failure) { if (box instanceof LongBox) ((LongBox) box).failure = failure; else ((DoubleBox) box).failure = failure; }
    static Object box(int kind, long value, double decimal, int[] marker) {
        if (kind == 0) { LongBox box = new LongBox(value); box.marker = marker; return box; }
        DoubleBox box = new DoubleBox(decimal); box.marker = marker; return box;
    }
    static void last(int kind, Core core, long value, double decimal) {
        if (kind == 0) check(core.lastLong == value);
        else check(Double.doubleToRawLongBits(core.lastDouble) == Double.doubleToRawLongBits(decimal));
    }
    public static void main(String[] args) throws Throwable {
        boolean rebuilt = args.length == 1 && args[0].equals("rebuilt");
        long[] edges = {0, Long.MIN_VALUE, Long.MAX_VALUE, -1, 1, 0x80000000L, 0x7ff8000000001234L, 0xfff8000000004321L};
        long[] bits = {0, Long.MIN_VALUE, 0x3ff0000000000000L, 0xbff0000000000000L, 0x7ff0000000000000L, 0xfff0000000000000L, 0x7ff8000000001234L, 0xfff8000000004321L};
        for (int edge = 0; edge < edges.length; edge++) for (int route = 0; route < 6; route++) for (int kind = 0; kind < 2; kind++) {
            long value = edges[edge]; double decimal = Double.longBitsToDouble(bits[edge]); Core core = new Core(value, decimal);
            int[] alias = {19}; core.marker = alias; Object input = box(kind, value, decimal, alias);
            for (int step = 0; step < 2; step++) {
                int before = alias[0]; boolean expected = kind == 0 ? value == core.expectedLong : decimal == core.expectedDouble;
                check(invoke(route, kind, core, input) == expected); last(kind, core, value, decimal);
                state(boxCalls(input) == step + 1); state(core.calls == step + 1);
                state(boxObserved(input) == before); state(core.observed == before + 3); state(alias[0] == before + 10);
                same(core.marker, alias); same(boxMarker(input), alias);
                core.expectedLong ^= 0x80000000L; core.expectedDouble = 19.25;
            }
            for (Object wrong : new Object[] {null, new Object(), kind == 0 ? new DoubleBox(decimal) : new LongBox(value)}) {
                int before = alias[0]; check(!invoke(route, kind, core, wrong)); state(alias[0] == before); state(core.calls == 2); state(boxCalls(input) == 2);
            }
            for (Object tag : new Object[] {input, alias, core, null}) { same(core.echo(tag, input), tag); same(((Api<String>) core).echo(tag, core), tag); }
        }
        for (int route = 0; route < 6; route++) for (int kind = 0; kind < 2; kind++) for (boolean targetFailure : new boolean[] {false, true}) {
            Core core = new Core(-17, -0.0); int[] alias = {7}; core.marker = alias; Object input = box(kind, -17, -0.0, alias);
            RuntimeException sentinel = new IllegalArgumentException("owned ordered failure");
            if (targetFailure) core.failure = sentinel; else boxFailure(input, sentinel);
            try { invoke(route, kind, core, input); throw new AssertionError("missing ordered failure"); }
            catch (RuntimeException failure) { failures++; same(failure, sentinel); }
            state(boxCalls(input) == 1); state(boxObserved(input) == 7); state(core.calls == (targetFailure ? 1 : 0));
            state(alias[0] == (targetFailure ? 17 : 10));
            if (targetFailure) { state(core.observed == 10); last(kind, core, -17, -0.0); }
            same(core.marker, alias); same(boxMarker(input), alias);
            try { invoke(route, kind, null, input); throw new AssertionError("missing null receiver"); }
            catch (NullPointerException expected) { failures++; }
            state(boxCalls(input) == 1); state(core.calls == (targetFailure ? 1 : 0));
        }
        for (String name : names) {
            Method bridge = Core.class.getDeclaredMethod(name, Object.class);
            check(bridge.isBridge() == !rebuilt); check(!bridge.isSynthetic()); check(Modifier.isFinal(bridge.getModifiers()));
            same(bridge.getReturnType(), boolean.class); same(bridge.getGenericParameterTypes()[0], Object.class);
        }
        Type[] interfaces = Core.class.getGenericInterfaces(); check(interfaces.length == 2);
        ParameterizedType edge = (ParameterizedType) interfaces[0]; same(edge.getRawType(), Api.class); same(edge.getActualTypeArguments()[0], String.class); same(interfaces[1], Marker.class);
        for (Class<?> declaration : new Class<?>[] {Api.class, Core.class}) {
            Method echo = declaration.getDeclaredMethod("echo", Object.class, Object.class); TypeVariable<?>[] formals = echo.getTypeParameters(); check(formals.length == 2);
            for (TypeVariable<?> formal : formals) { same(formal.getGenericDeclaration(), echo); same(formal.getBounds()[0], Object.class); }
            check(echo.getGenericParameterTypes()[0].equals(formals[0])); check(echo.getGenericParameterTypes()[1].equals(formals[1])); check(echo.getGenericReturnType().equals(formals[0]));
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("original-type-guard-unboxing-and-scalar-dispatch=retained,false-inputs-and-ordered-effects=matched,complete-interface-signature=unchanged,bridge-flags=reported");
    }
}
