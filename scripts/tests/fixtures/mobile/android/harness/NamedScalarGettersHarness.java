import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;

public class NamedScalarGettersHarness {
    static int values, identities, states, failures;
    static final String[] names = {"size", "span", "ratio", "weight"};
    static final Class<?>[] types = {int.class, long.class, float.class, double.class};
    static void check(boolean yes) { values++; if (!yes) throw new AssertionError("value " + values); }
    static void same(Object a, Object b) { identities++; if (a != b) throw new AssertionError("identity " + identities); }
    static void state(boolean yes) { states++; if (!yes) throw new AssertionError("state " + states); }
    static Object invoke(int route, int kind, Core core) throws Throwable {
        if (route <= 2) {
            if (route == 0) {
                switch (kind) { case 0: return core.size(); case 1: return core.span(); case 2: return core.ratio(); default: return core.weight(); }
            }
            Api<?> api = core;
            if (route == 2) api = (Api) core;
            switch (kind) { case 0: return api.size(); case 1: return api.span(); case 2: return api.ratio(); default: return api.weight(); }
        }
        Class<?> owner = route == 3 ? Core.class : Api.class;
        try {
            if (route != 5) return owner.getMethod(names[kind]).invoke(core);
            return MethodHandles.publicLookup().findVirtual(Api.class, names[kind], MethodType.methodType(types[kind])).invoke(core);
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    static void result(int kind, Object value, Core core) {
        switch (kind) {
            case 0: check(((Integer) value).intValue() == core.integer); break;
            case 1: check(((Long) value).longValue() == core.wide); break;
            case 2: check(Float.floatToRawIntBits(((Float) value).floatValue()) == Float.floatToRawIntBits(core.narrow)); break;
            default: check(Double.doubleToRawLongBits(((Double) value).doubleValue()) == Double.doubleToRawLongBits(core.decimal));
        }
    }
    public static void main(String[] args) throws Throwable {
        boolean rebuilt = args.length == 1 && args[0].equals("rebuilt");
        int[] bits = {0, 0x80000000, 0x3f800000, 0xbf800000, 0x7f800000, 0xff800000, 0x7fc01234, 0xffc04321};
        long[] wideBits = {0, Long.MIN_VALUE, 0x3ff0000000000000L, 0xbff0000000000000L, 0x7ff0000000000000L, 0xfff0000000000000L, 0x7ff8000000001234L, 0xfff8000000004321L};
        for (int edge = 0; edge < bits.length; edge++) for (int route = 0; route < 6; route++) for (int kind = 0; kind < 4; kind++) {
            Core core = new Core(bits[edge], wideBits[edge], Float.intBitsToFloat(bits[edge]), Double.longBitsToDouble(wideBits[edge]));
            int[] alias = {edge + 19}; core.marker = alias;
            for (int step = 0; step < 2; step++) {
                int before = alias[0]; result(kind, invoke(route, kind, core), core);
                state(core.calls == step + 1); state(core.observed == before); state(alias[0] == before + 3); same(core.marker, alias);
                core.integer ^= 0x55; core.wide ^= 0x80000000L;
                core.narrow = Float.intBitsToFloat(bits[(edge + step + 1) % bits.length]);
                core.decimal = Double.longBitsToDouble(wideBits[(edge + step + 1) % bits.length]);
            }
            for (Object input : new Object[] {core, alias, "owned", null}) { same(core.echo(input, core), input); same(((Api<String>) core).echo(input, alias), input); }
        }
        for (int route = 0; route < 6; route++) for (int kind = 0; kind < 4; kind++) {
            Core core = new Core(-17, Long.MAX_VALUE, -0.0f, -0.0); int[] alias = {7}; core.marker = alias;
            RuntimeException sentinel = new IllegalStateException("owned scalar failure"); core.failure = sentinel;
            try { invoke(route, kind, core); throw new AssertionError("missing target failure"); }
            catch (RuntimeException failure) { failures++; same(failure, sentinel); }
            state(core.calls == 1); state(core.observed == 7); state(alias[0] == 10); same(core.marker, alias);
            try { invoke(route, kind, null); throw new AssertionError("missing null receiver"); }
            catch (NullPointerException expected) { failures++; }
            state(core.calls == 1); state(alias[0] == 10);
        }
        for (int kind = 0; kind < 4; kind++) {
            Method bridge = Core.class.getDeclaredMethod(names[kind]); check(bridge.isBridge() == !rebuilt); check(bridge.isSynthetic() == !rebuilt);
            same(bridge.getReturnType(), types[kind]); same(bridge.getGenericReturnType(), types[kind]);
            same(Api.class.getDeclaredMethod(names[kind]).getReturnType(), types[kind]);
        }
        Type[] interfaces = Core.class.getGenericInterfaces(); check(interfaces.length == 2);
        ParameterizedType edge = (ParameterizedType) interfaces[0]; same(edge.getRawType(), Api.class); same(edge.getActualTypeArguments()[0], String.class); same(interfaces[1], Marker.class);
        for (Class<?> declaration : new Class<?>[] {Api.class, Core.class}) {
            Method echo = declaration.getDeclaredMethod("echo", Object.class, Object.class); TypeVariable<?>[] formals = echo.getTypeParameters(); check(formals.length == 2);
            for (TypeVariable<?> formal : formals) { same(formal.getGenericDeclaration(), echo); same(formal.getBounds()[0], Object.class); }
            check(echo.getGenericParameterTypes()[0].equals(formals[0])); check(echo.getGenericParameterTypes()[1].equals(formals[1])); check(echo.getGenericReturnType().equals(formals[0]));
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("original-scalar-targets-effects-bits-and-failure-order=retained,complete-interface-argument=unchanged,bridge-flags=reported");
    }
}
