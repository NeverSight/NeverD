package probe;
import fixture.*;
import java.io.IOException;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class MixedInterfaceSuperHarness {
    static void check(boolean value) {
        if (!value) throw new AssertionError("owned mixed interface super mismatch");
    }
    @SuppressWarnings("unchecked")
    static <E extends Throwable> void raise(Throwable failure) throws E { throw (E) failure; }
    static final class Future extends Values {
        Future(long value) { super(value); }
        @Override public int measure(int value) {
            Trace.hit(7);
            int result = super.measure(value);
            Trace.hit(8);
            return result ^ 37;
        }
        @Override public long measure(long value) {
            Trace.hit(7);
            long result = super.measure(value);
            Trace.hit(8);
            return result ^ 37;
        }
    }
    static long call(int route, boolean wide, Method method, Api owner, Context context, Input input, long value) throws Throwable {
        if (route == 0) return wide ? Core.call(owner, context, input, value) : Core.call(owner, context, input, (int) value);
        if (route == 1) return wide ? MixedInterfaceSuperCaller.call(owner, context, input, value)
            : MixedInterfaceSuperCaller.call(owner, context, input, (int) value);
        Trace.hit(6);
        long result;
        try {
            if (route == 2 || route == 4) {
                Method selected = route == 2 ? Api.class.getMethod("measure", Context.class, Input.class, wide ? long.class : int.class) : method;
                Object scalar = wide ? (Object) Long.valueOf(value) : Integer.valueOf((int) value);
                result = ((Number) selected.invoke(owner, context, input, scalar)).longValue();
            } else if (route == 3 || route == 5) {
                MethodType type = MethodType.methodType(wide ? long.class : int.class, Context.class, Input.class, wide ? long.class : int.class);
                MethodHandle handle = route == 3 ? MethodHandles.publicLookup().findVirtual(Api.class, "measure", type)
                    : MethodHandles.lookup().unreflect(method).asType(type.insertParameterTypes(0, Api.class));
                result = wide ? (long) handle.invokeExact(owner, context, input, value)
                    : (int) handle.invokeExact(owner, context, input, (int) value);
            } else result = wide ? MixedInterfaceSuperCaller.direct(owner, context, input, value)
                : MixedInterfaceSuperCaller.direct(owner, context, input, (int) value);
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
        Trace.hit(0);
        return result;
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        boolean wide = Boolean.parseBoolean(args[2]), bridge = Boolean.parseBoolean(args[3]);
        Class<?> ownerClass = Core.make().getClass();
        check(Modifier.isFinal(ownerClass.getModifiers()) && !Modifier.isPublic(ownerClass.getModifiers()));
        check(ownerClass.getSuperclass() == Host.Base.class && Modifier.isStatic(Host.Base.class.getModifiers()));
        check(java.util.Arrays.equals(ownerClass.getInterfaces(), new Class<?>[]{Api.class, SideA.class, SideB.class, SideC.class}));
        check(java.util.Arrays.equals(Host.Base.class.getInterfaces(), new Class<?>[]{Shared.class}));
        Method method = ownerClass.getDeclaredMethod("measure", Context.class, Input.class, wide ? long.class : int.class);
        check(method.isBridge() == bridge && !method.isSynthetic() && Modifier.isPublic(method.getModifiers()));
        check(method.getReturnType() == (wide ? long.class : int.class) && method.getTypeParameters().length == 0 && method.getExceptionTypes().length == 0);
        check(Api.class.getMethod("measure", Context.class, Input.class, wide ? long.class : int.class).isDefault());
        method.setAccessible(true);
        for (int attempt = 0; attempt < 2; ++attempt) {
            Trace.beginAction = Trace.contextAction = Trace.baseAction = Trace.inputAction = Trace.afterAction = null;
            boolean future = attempt == 1;
            long raw = (future ? 0x7ff8123456abcdefL : 0x8123456789abcdefL) ^ mode;
            long value = wide ? raw : (int) raw;
            Values ctx = new Values(0x3141592653589793L);
            Values arg = future ? new Future(0x2718281828459045L) : mode == 0 ? ctx : new Values(0x2718281828459045L);
            Context context = mode == 1 ? null : ctx;
            Input input = mode == 2 ? null : arg;
            Api owner = mode == 10 ? null : Core.make();
            Api other = Core.make();
            Values otherContext = new Values(0x1155993377aacceeL), otherInput = new Values(0x6633221155aaee77L);
            Values sentinelValue = new Values(0);
            Object sentinel = new Object();
            long empty = 0x76543210abcdef01L;
            Trace.events = 0;
            Trace.parent = Trace.contextOwner = Trace.baseOwner = Trace.inputOwner = sentinel;
            Trace.context = sentinelValue;
            Trace.input = sentinelValue;
            Trace.parameter = Trace.observed = Trace.result = empty;
            Trace.baseValue = 0x6dbe1729;
            int[] calls = {0};
            long[] nested = {empty};
            Throwable failure = mode == 3 || mode == 6 ? new IOException("owned checked")
                : mode == 5 || mode == 11 ? new AssertionError("owned error") : new IllegalStateException("owned runtime");
            Runnable action = () -> { calls[0]++; raise(failure); };
            if (mode == 3) Trace.beginAction = action;
            if (mode == 4) Trace.contextAction = action;
            if (mode == 5) Trace.baseAction = action;
            if (mode == 6) Trace.inputAction = action;
            if (mode == 7 || mode == 11) Trace.afterAction = action;
            if (mode == 8) Trace.beginAction = () -> {
                calls[0]++;
                ctx.seed ^= 0x1122334455667788L;
                arg.seed ^= 0x8877665544332211L;
                Trace.baseValue = 0x33557799;
            };
            if (mode == 9) Trace.beginAction = () -> {
                calls[0]++;
                Trace.beginAction = null;
                nested[0] = wide ? Core.call(other, otherContext, otherInput, ~value)
                    : Core.call(other, otherContext, otherInput, (int) ~value);
                long expected = ~value ^ otherContext.seed ^ Trace.baseValue ^ otherInput.seed;
                check(nested[0] == (wide ? expected : (int) expected));
            };
            long result = empty;
            Throwable caught = null;
            try { result = call(route, wide, method, owner, context, input, value); }
            catch (Throwable error) { caught = error; }
            boolean begun = mode != 10, offset = begun && mode != 1 && mode != 3;
            boolean base = offset && mode != 4, selected = base && mode != 2 && mode != 5;
            boolean after = selected && mode != 6, tail = after && mode != 7 && mode != 11;
            String events = "6";
            if (begun) events += "1";
            if (mode == 9) events += "6123450";
            if (offset) events += "2";
            if (base) events += "3";
            if (selected) events += future ? "74" : "4";
            if (after && future) events += "8";
            if (after) events += "5";
            if (tail) events += "0";
            check(Trace.events == Long.parseLong(events));
            check(Trace.parent == (begun ? mode == 9 ? other : owner : sentinel));
            check(Trace.context == (begun ? mode == 9 ? otherContext : context : sentinelValue));
            check(Trace.input == (begun ? mode == 9 ? otherInput : input : sentinelValue));
            long recorded = mode == 9 ? wide ? ~value : (int) ~value : value;
            check(Trace.parameter == (begun ? recorded : empty));
            check(Trace.contextOwner == (offset ? ctx : sentinel));
            check(Trace.baseOwner == (base ? owner : sentinel));
            check(Trace.inputOwner == (selected ? arg : sentinel));
            long argument = value ^ ctx.seed ^ Trace.baseValue;
            if (!wide) argument = (int) argument;
            long expected = argument ^ arg.seed ^ (future ? 37 : 0);
            if (!wide) expected = (int) expected;
            check(Trace.observed == (selected ? argument : empty));
            check(Trace.result == (after ? expected : empty));
            check(calls[0] == (mode == 0 || mode == 1 || mode == 2 || mode == 10 ? 0 : 1));
            if (tail) check(caught == null && result == expected);
            else if (mode == 1 || mode == 2 || mode == 10) check(caught instanceof NullPointerException && result == empty);
            else check(caught == failure && result == empty);
            if (mode == 9) check(nested[0] != empty);
        }
        System.out.println("attempts=2,mixed-interface-super=matched");
    }
}
