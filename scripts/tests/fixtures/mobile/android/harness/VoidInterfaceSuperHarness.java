package probe;
import fixture.*;
import java.io.IOException;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class VoidInterfaceSuperHarness {
    static void check(boolean value) {
        if (!value) throw new AssertionError("owned void interface super mismatch");
    }
    @SuppressWarnings("unchecked")
    static <E extends Throwable> void raise(Throwable failure) throws E { throw (E) failure; }
    static void call(int route, Method method, Api owner) throws Throwable {
        if (route == 0) { Core.call(owner); return; }
        if (route == 1) { VoidInterfaceSuperCaller.call(owner); return; }
        Trace.hit(6);
        try {
            if (route == 2 || route == 4) {
                Method selected = route == 2 ? Api.class.getMethod("signal") : method;
                check(selected.invoke(owner) == null);
            } else if (route == 3 || route == 5) {
                MethodType type = MethodType.methodType(void.class);
                MethodHandle handle = route == 3 ? MethodHandles.publicLookup().findVirtual(Api.class, "signal", type)
                    : MethodHandles.lookup().unreflect(method).asType(type.insertParameterTypes(0, Api.class));
                handle.invokeExact(owner);
            } else VoidInterfaceSuperCaller.direct(owner);
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
        Trace.hit(0);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        boolean bridge = Boolean.parseBoolean(args[2]);
        Class<?> ownerClass = Core.make().getClass();
        check(Modifier.isFinal(ownerClass.getModifiers()) && !Modifier.isPublic(ownerClass.getModifiers()));
        check(ownerClass.getSuperclass() == Host.Base.class && Modifier.isStatic(Host.Base.class.getModifiers()));
        check(java.util.Arrays.equals(ownerClass.getInterfaces(), new Class<?>[]{SideA.class, Api.class, SideB.class, SideC.class}));
        check(java.util.Arrays.equals(Host.Base.class.getInterfaces(), new Class<?>[]{Shared.class}));
        Method method = ownerClass.getDeclaredMethod("signal");
        check(method.isBridge() == bridge && !method.isSynthetic() && Modifier.isPublic(method.getModifiers()));
        check(method.getReturnType() == void.class && method.getTypeParameters().length == 0 && method.getExceptionTypes().length == 0);
        check(Api.class.getMethod("signal").isDefault());
        method.setAccessible(true);
        for (int attempt = 0; attempt < 2; ++attempt) {
            Trace.beginAction = Trace.baseAction = Trace.afterAction = null;
            long seed = (attempt == 0 ? 0x7ff8123456abcdefL : 0x8123456789abcdefL) ^ mode;
            Trace.seed = seed;
            Api allocated = Core.make(), other = Core.make();
            Host.Base state = (Host.Base) allocated, otherState = (Host.Base) other;
            check(state.value == seed && otherState.value == seed);
            Api owner = mode == 1 ? null : allocated;
            Object sentinel = new Object();
            long empty = 0x76543210abcdef01L;
            Trace.events = 0;
            Trace.parent = Trace.baseOwner = Trace.completion = sentinel;
            Trace.before = Trace.observed = empty;
            long initialDelta = 0x3141592653589793L;
            Trace.delta = initialDelta;
            int[] calls = {0};
            long[] nested = {empty};
            Throwable failure = mode == 2 || mode == 8 ? new IOException("owned checked")
                : mode == 4 || mode == 7 ? new AssertionError("owned error") : new IllegalStateException("owned runtime");
            Runnable action = () -> { calls[0]++; raise(failure); };
            if (mode == 2 || mode == 7) Trace.beginAction = action;
            if (mode == 3 || mode == 8) Trace.baseAction = action;
            if (mode == 4 || mode == 9) Trace.afterAction = action;
            if (mode == 5) Trace.beginAction = () -> {
                calls[0]++;
                state.value ^= 0x1122334455667788L;
                Trace.delta ^= 0x8877665544332211L;
            };
            if (mode == 6) Trace.beginAction = () -> {
                calls[0]++;
                Trace.beginAction = null;
                Core.call(other);
                nested[0] = otherState.value;
                check(nested[0] == seed + initialDelta);
            };
            if (mode == 10) Trace.baseAction = () -> {
                calls[0]++;
                state.value ^= 0x4433221188776655L;
                Trace.delta = ~initialDelta;
            };
            if (mode == 11) Trace.afterAction = () -> {
                calls[0]++;
                state.value ^= 0x5566778811223344L;
            };
            boolean returned = false;
            Throwable caught = null;
            try { call(route, method, owner); returned = true; }
            catch (Throwable error) { caught = error; }
            boolean begun = mode != 1, base = begun && mode != 2 && mode != 7;
            boolean after = base && mode != 3 && mode != 8;
            boolean tail = after && mode != 4 && mode != 9;
            String events = "6";
            if (begun) events += "1";
            if (mode == 6) events += "61230";
            if (base) events += "2";
            if (after) events += "3";
            if (tail) events += "0";
            check(Trace.events == Long.parseLong(events));
            check(Trace.parent == (begun ? mode == 6 ? other : owner : sentinel));
            check(Trace.baseOwner == (base ? owner : sentinel));
            check(Trace.completion == (after ? owner : sentinel));
            long before = mode == 5 ? seed ^ 0x1122334455667788L : seed;
            long value = mode == 10 ? before ^ 0x4433221188776655L : before;
            long delta = mode == 5 ? initialDelta ^ 0x8877665544332211L : mode == 10 ? ~initialDelta : initialDelta;
            long observed = value + delta;
            check(Trace.before == (base ? before : empty));
            check(Trace.observed == (after ? observed : empty));
            check(state.value == (after ? mode == 11 ? observed ^ 0x5566778811223344L : observed : before));
            check(otherState.value == (mode == 6 ? seed + initialDelta : seed));
            check(calls[0] == (mode == 0 || mode == 1 ? 0 : 1));
            if (tail) check(caught == null && returned);
            else if (mode == 1) check(caught instanceof NullPointerException && !returned);
            else check(caught == failure && !returned);
            if (mode == 6) check(nested[0] != empty);
        }
        System.out.println("attempts=2,void-interface-super=matched");
    }
}
