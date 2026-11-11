import fixture.Api;
import fixture.Base;
import fixture.Core;
import fixture.Caller;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

public final class FinalVoidReturnSlotsHarness {
    private static int values, identities, states, failures;
    private static Method VOID, SCALAR, PUT;
    private static MethodHandle VH, SH, PH;
    private static void same(Object expected, Object actual) {
        if (expected != actual) throw new AssertionError("identity changed"); identities++;
    }
    private static void state(int expected, int actual) {
        if (expected != actual) throw new AssertionError("state changed"); states++;
    }
    private static void value(boolean expected, boolean actual) {
        if (expected != actual) throw new AssertionError("value changed"); values++;
    }
    private static void put(int route, Core<Object> core, Object item) throws Throwable {
        try {
            switch (route) {
                case 0: Caller.put(core, item); return;
                case 1: VOID.invoke(core, item); return;
                case 2: VH.invokeExact((Core) core, item); return;
                case 3: PUT.invoke(null, core, item); return;
                case 4: PH.invokeExact((Core) core, item); return;
                case 5: core.twice(item); return;
                default: throw new AssertionError();
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static boolean scalar(int route, Core<Object> core, Object item) throws Throwable {
        try {
            switch (route) {
                case 0: return ((Base<Object>) core).add(item);
                case 1: return ((Api<Object>) core).add(item);
                case 2: return ((Api) core).add(item);
                case 3: return (Boolean) SCALAR.invoke(core, item);
                case 4: return (boolean) SH.invokeExact((Base) core, item);
                case 5: return ((Api<Object>) core).insert(item);
                case 6: return Caller.scalar(core, item);
                default: throw new AssertionError();
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void metadata(String name) throws Exception {
        if (Core.class.getTypeParameters().length != 1 || Base.class.getTypeParameters().length != 1
                || !VOID.getGenericParameterTypes()[0].equals(Core.class.getTypeParameters()[0])
                || !SCALAR.getGenericParameterTypes()[0].equals(Base.class.getTypeParameters()[0])
                || java.util.Arrays.stream(Core.class.getDeclaredMethods()).filter(m -> java.lang.reflect.Modifier.isPublic(m.getModifiers())).count() != 2 || VOID.getReturnType() != void.class
                || VOID.isBridge() || VOID.isSynthetic() || SCALAR.getDeclaringClass() != Base.class
                || Core.class.getMethod("add", Object.class).getReturnType() != (name.equals("add") ? void.class : boolean.class))
            throw new AssertionError("original full scope or inherited ownership changed");
    }
    public static void main(String[] args) throws Throwable {
        VOID = Core.class.getDeclaredMethod(args[0], Object.class);
        SCALAR = Base.class.getMethod("add", Object.class);
        PUT = Caller.class.getMethod("put", Core.class, Object.class);
        VH = MethodHandles.lookup().findVirtual(Core.class, args[0], MethodType.methodType(void.class, Object.class));
        SH = MethodHandles.lookup().findVirtual(Base.class, "add", MethodType.methodType(boolean.class, Object.class));
        PH = MethodHandles.lookup().findStatic(Caller.class, "put", MethodType.methodType(void.class, Core.class, Object.class));
        metadata(args[0]);
        for (int route = 0; route < 6; route++) {
            for (int kind = 0; kind < 4; kind++) {
                Object item = kind == 0 ? null : kind == 1 ? new Object() : kind == 2 ? "owned" : new int[]{7};
                Core<Object> core = new Core<Object>();
                int count = route == 5 ? 2 : 1, marker = route == 5 ? 4 : 1;
                put(route, core, item); state(count, core.voidCalls); state(marker, core.voidState);
                state(0, core.scalarCalls); same(item, core.voidInput);
                if (item instanceof int[]) state(7 + 5 * count, ((int[]) item)[0]);
                put(route, core, item); state(2 * count, core.voidCalls); state(route == 5 ? 40 : 4, core.voidState);
                state(0, core.scalarState); same(item, core.voidInput);
                if (item instanceof int[]) state(7 + 10 * count, ((int[]) item)[0]);
            }
            Core<Object> core = new Core<Object>(); int[] item = {11};
            RuntimeException failure = new RuntimeException("owned-void"); core.voidFailure = failure;
            try { put(route, core, item); throw new AssertionError("missing void exception"); }
            catch (Throwable error) { same(failure, error); failures++; }
            state(1, core.voidCalls); state(1, core.voidState); state(16, item[0]); same(item, core.voidInput);
            try { put(route, null, item); throw new AssertionError("null void receiver accepted"); }
            catch (NullPointerException error) { failures++; }
            state(16, item[0]); state(0, core.scalarCalls);
        }
        for (int route = 0; route < 7; route++) {
            for (int kind = 0; kind < 4; kind++) {
                Object item = kind == 0 ? null : kind == 1 ? new Object() : kind == 2 ? "owned" : new int[]{7};
                Core<Object> core = new Core<Object>();
                value(item != null, scalar(route, core, item)); state(1, core.scalarCalls); state(1, core.scalarState);
                state(0, core.voidCalls); same(item, core.scalarInput);
                if (item instanceof int[]) state(9, ((int[]) item)[0]);
                value(item != null, scalar(route, core, item)); state(2, core.scalarCalls); state(3, core.scalarState);
                state(0, core.voidState); same(item, core.scalarInput);
                if (item instanceof int[]) state(11, ((int[]) item)[0]);
            }
            Core<Object> core = new Core<Object>(); int[] item = {11};
            RuntimeException failure = new RuntimeException("owned-scalar"); core.scalarFailure = failure;
            try { scalar(route, core, item); throw new AssertionError("missing scalar exception"); }
            catch (Throwable error) { same(failure, error); failures++; }
            state(1, core.scalarCalls); state(1, core.scalarState); state(13, item[0]); same(item, core.scalarInput);
            try { scalar(route, null, item); throw new AssertionError("null scalar receiver accepted"); }
            catch (NullPointerException error) { failures++; }
            state(13, item[0]); state(0, core.voidCalls);
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("distinct-runtime-return-slots-and-generic-provider=retained,void-source-name=reported");
    }
}
