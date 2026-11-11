import fixture.Base;
import fixture.Parent;
import fixture.Core;
import fixture.Pair;
import fixture.MutablePair;
import fixture.Caller;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;

public final class InheritedWildcardParametersHarness {
    private static int values, identities, states, failures;
    private static Method OWN, ROOT;
    private static MethodHandle OH, RH;
    private static void same(Object expected, Object actual) {
        if (expected != actual) throw new AssertionError("identity changed"); identities++;
    }
    private static void state(int expected, int actual) {
        if (expected != actual) throw new AssertionError("state changed: " + expected + "/" + actual); states++;
    }
    private static void value(boolean expected, boolean actual) {
        if (expected != actual) throw new AssertionError("return changed"); values++;
    }
    private static boolean call(int route, Core<Object,Object> core, MutablePair pair) throws Throwable {
        try {
            switch (route) {
                case 0: return core.entry(pair);
                case 1: return Caller.call(core, pair);
                case 2: return (Boolean) OWN.invoke(core, pair);
                case 3: return (boolean) OH.invokeExact((Core) core, (Pair) pair);
                case 4: return (Boolean) ROOT.invoke(core, pair);
                case 5: return (boolean) RH.invokeExact((Parent) core, (Pair) pair);
                case 6: return ((Parent) core).remove((Object) pair);
                case 7: return core.call(pair);
                default: throw new AssertionError();
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void fields(Core<Object,Object> core, Object input, Object key, Object val) {
        same(input, core.observed); same(key, core.key); same(val, core.value);
    }
    private static void effects(Core<Object,Object> core, MutablePair pair, int calls, int marker,
                                int keys, int vals, int pairMarker) {
        state(calls, core.calls); state(marker, core.marker); state(0, core.baseCalls);
        state(keys, pair.keyCalls); state(vals, pair.valueCalls); state(pairMarker, pair.marker);
    }
    private static void metadata(boolean original) throws Exception {
        Object parameter = OWN.getGenericParameterTypes()[0];
        if ((parameter instanceof ParameterizedType) != original
                || (!original && parameter != Pair.class)
                || !(ROOT.getGenericParameterTypes()[0] instanceof ParameterizedType)
                || Core.class.getTypeParameters().length != 2 || Parent.class.getTypeParameters().length != 3
                || Core.class.getMethod("remove", Object.class).getDeclaringClass() != Parent.class
                || OWN.isBridge() || OWN.isSynthetic()
                || java.util.Arrays.stream(Core.class.getDeclaredMethods()).filter(m -> java.lang.reflect.Modifier.isPublic(m.getModifiers())).count() != 2)
            throw new AssertionError("scope, parameter Signature or original declaration ownership changed");
    }
    public static void main(String[] args) throws Throwable {
        OWN = Core.class.getDeclaredMethod("remove", Pair.class);
        ROOT = Parent.class.getDeclaredMethod("remove", Pair.class);
        OH = MethodHandles.lookup().findVirtual(Core.class, "remove", MethodType.methodType(boolean.class, Pair.class));
        RH = MethodHandles.lookup().findVirtual(Parent.class, "remove", MethodType.methodType(boolean.class, Pair.class));
        metadata(args[0].equals("original"));
        for (int route = 0; route < 8; route++) {
            for (int kind = 0; kind < 4; kind++) {
                Core<Object,Object> core = new Core<Object,Object>(); MutablePair pair = new MutablePair();
                pair.key = kind == 0 ? null : kind == 2 || kind == 3 ? new int[]{11} : new Object();
                pair.value = kind == 1 ? null : kind == 3 ? pair.key : kind == 2 ? new int[]{13} : new Object();
                for (int count = 1; count <= 2; count++) {
                    value(pair.key != null && pair.value != null, call(route, core, pair));
                    fields(core, pair, pair.key, pair.value); effects(core, pair, count, count == 1 ? 1 : 4, count, count, count == 1 ? 5 : 50);
                    if (pair.key instanceof int[]) state(11 + (kind == 3 ? 12 : 5) * count, ((int[]) pair.key)[0]);
                    if (pair.value instanceof int[]) state((kind == 3 ? 11 : 13) + (kind == 3 ? 12 : 7) * count, ((int[]) pair.value)[0]);
                }
            }
            for (int mode = 0; mode < 4; mode++) {
                Core<Object,Object> core = new Core<Object,Object>(); MutablePair pair = new MutablePair();
                int[] key = {11}, val = {13}; pair.key = key; pair.value = val;
                RuntimeException failure = new RuntimeException("owned-failure");
                if (mode == 0) pair.keyFailure = failure;
                if (mode == 1) pair.valueFailure = failure;
                if (mode == 2) core.failure = failure;
                try { call(route, mode == 3 ? null : core, pair); throw new AssertionError("missing failure"); }
                catch (Throwable error) {
                    if (mode == 3) { if (!(error instanceof NullPointerException)) throw error; }
                    else same(failure, error);
                    failures++;
                }
                fields(core, mode == 3 ? null : pair, mode == 0 || mode == 3 ? null : key, mode == 2 ? val : null);
                effects(core, pair, mode == 3 ? 0 : 1, mode == 3 ? 0 : 1, mode == 3 ? 0 : 1, mode == 1 || mode == 2 ? 1 : 0,
                        mode == 3 ? 0 : mode == 0 ? 1 : 5);
                state(mode == 2 ? 16 : 11, key[0]); state(mode == 2 ? 20 : 13, val[0]);
            }
            if (route < 6) {
                Core<Object,Object> core = new Core<Object,Object>();
                try { call(route, core, null); throw new AssertionError("typed null accepted"); }
                catch (NullPointerException error) { failures++; }
                state(1, core.calls); state(1, core.marker); state(0, core.baseCalls); fields(core, null, null, null);
            }
        }
        for (int route = 0; route < 2; route++)
            for (Object input : new Object[]{null, new Object(), "owned"}) {
                Core<Object,Object> core = new Core<Object,Object>();
                value(false, route == 0 ? ((Parent) core).remove(input) : core.call(input));
                state(0, core.calls); state(0, core.marker); state(0, core.baseCalls); fields(core, null, null, null);
            }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("complete-bound-and-original-guarded-dispatch=retained,source-parameter-erasure=reported");
    }
}
