import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class DelegationContinuationHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("delegation continuation changed"); }
    static Core create(int variant, int route, Object token, boolean flag, Holder holder) throws Throwable {
        if (route == 0) {
            if (variant == 0) return new Core(token, flag);
            if (variant == 1) return new Core(token, flag, holder);
            if (variant == 2) return new Core(token, flag, holder, (byte) 0);
            if (variant == 3) return new Core(token, flag, holder, (char) 0);
            if (variant == 4) return new Core(token, flag, holder, (short) 0);
            if (variant == 5) return new Core(token, flag, holder, 0);
            return new Core(token, flag, holder, 0L);
        }
        if (route == 1) return DelegationContinuationCaller.create(variant, token, flag, holder);
        Class<?>[] types = variant == 0 ? new Class<?>[]{Object.class, boolean.class}
            : variant == 1 ? new Class<?>[]{Object.class, boolean.class, Holder.class}
            : new Class<?>[]{Object.class, boolean.class, Holder.class,
                variant == 2 ? byte.class : variant == 3 ? char.class : variant == 4 ? short.class
                : variant == 5 ? int.class : long.class};
        if (route == 2) {
            Object marker = variant == 2 ? (Object) Byte.valueOf((byte) 0)
                : variant == 3 ? (Object) Character.valueOf((char) 0) : variant == 4 ? (Object) Short.valueOf((short) 0)
                : variant == 5 ? (Object) Integer.valueOf(0) : Long.valueOf(0L);
            try { return Core.class.getConstructor(types).newInstance(variant == 0
                ? new Object[]{token, flag} : variant == 1 ? new Object[]{token, flag, holder}
                : new Object[]{token, flag, holder, marker}); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        MethodHandle handle = MethodHandles.publicLookup().findConstructor(Core.class, MethodType.methodType(void.class, types));
        if (variant == 0) return (Core) handle.invokeExact(token, flag);
        if (variant == 1) return (Core) handle.invokeExact(token, flag, holder);
        if (variant == 2) return (Core) handle.invokeExact(token, flag, holder, (byte) 0);
        if (variant == 3) return (Core) handle.invokeExact(token, flag, holder, (char) 0);
        if (variant == 4) return (Core) handle.invokeExact(token, flag, holder, (short) 0);
        if (variant == 5) return (Core) handle.invokeExact(token, flag, holder, 0);
        return (Core) handle.invokeExact(token, flag, holder, 0L);
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        int[] failures = {0, 0, 0, 0, 0, 1, 2, 13, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0, 14, 15};
        Trace.failAt = failures[mode]; Trace.failure = new RuntimeException("owned delegation failure");
        Trace.initial = mode == 4 ? null : new Object();
        Trace.providerValue = mode == 18 ? null : new Object();
        boolean initialized = false, failed = false, flag = mode != 1;
        boolean providerInitialized = false, providerFailed = false;
        for (int attempt = 0; attempt < 2; attempt++) {
            Object token = mode == 2 ? null : new Object(), before = new Object();
            Trace.holder = new Holder(); Trace.holder.value = before;
            Holder holder = mode == 3 ? null : Trace.holder;
            Trace.events = 0; Trace.observed = 0; Trace.seen = null; Trace.inputToken = null; Trace.readValue = null;
            Trace.values = new Object[14]; Trace.tails = new Object[14]; Trace.numbers = new int[14];
            Trace.bits = new long[14]; Trace.flags = new boolean[14];
            Core result = null; Throwable failure = null;
            try { result = create(variant, route, token, flag, holder); }
            catch (Throwable problem) { failure = problem; }
            long trace = 0; int kind = 0, observed = 0;
            Object expectedHolder = before, value = variant == 0 ? token : null, tail = null;
            Object expectedInput = null, expectedRead = null, stored = null;
            int number = 0; long bits = 0; boolean entered = false, storedFlag = false;
            int[] stages = variant == 0 ? new int[]{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}
                : variant < 5 ? new int[]{1, 2, 13, -1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}
                : new int[]{1, 2, 14, 15, 13, -1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
            if (failed) kind = 3;
            else for (int stage : stages) {
                if (stage > 0 && stage <= 2 && initialized) continue;
                if (stage == 14 && providerFailed) { kind = 3; break; }
                if ((stage == 14 || stage == 15) && providerInitialized) continue;
                if (stage == 2) expectedHolder = Trace.initial;
                if (stage == 15) expectedHolder = Trace.providerValue;
                if (stage == -1) {
                    expectedInput = token;
                    if (holder == null) { kind = 4; break; }
                    value = expectedHolder; expectedRead = value;
                    continue;
                }
                if (stage == 4) entered = true;
                if (stage == 5) stored = value;
                if (stage == 6) { number = 17; bits = 0x8000000000000023L; }
                if (stage == 9) storedFlag = flag;
                if (stage == 12) tail = expectedHolder;
                if ((stage >= 4 && stage <= 7) || (stage >= 9 && stage <= 12)) {
                    observed |= 1 << stage;
                    require(Trace.values[stage] == stored && Trace.numbers[stage] == number
                        && Trace.bits[stage] == bits && Trace.flags[stage] == storedFlag && Trace.tails[stage] == tail);
                }
                trace = trace * 17 + stage;
                if (Trace.failAt == stage) {
                    kind = stage <= 2 || stage == 14 || stage == 15 ? 2 : 1;
                    if (stage <= 2) failed = true;
                    if (stage == 14 || stage == 15) providerFailed = true;
                    break;
                }
                if (stage == 2) initialized = true;
                if (stage == 15) providerInitialized = true;
            }
            require(Trace.events == trace && Trace.observed == observed && Trace.holder.value == expectedHolder
                && Trace.inputToken == expectedInput && Trace.readValue == expectedRead);
            if (entered) require(Trace.seen != null && Trace.seen.value == stored && Trace.seen.number == number
                && Trace.seen.bits == bits && Trace.seen.flag == storedFlag && Trace.seen.tail == tail);
            else require(Trace.seen == null);
            if (kind == 1) require(failure == Trace.failure);
            else if (kind == 2) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
            else if (kind == 3) require(failure instanceof NoClassDefFoundError);
            else if (kind == 4) require(failure instanceof NullPointerException);
            else require(failure == null && result == Trace.seen && entered);
            if (kind != 0) require(result == null);
            System.out.println("variant=" + variant + ",route=" + route + ",mode=" + mode
                + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
