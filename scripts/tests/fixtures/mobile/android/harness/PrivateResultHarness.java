import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class PrivateResultHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("private result contract"); }
    static Object call(int variant, int route, Owner.Worker caller, Owner.Worker receiver, String label,
                       Class<?>[] kinds, Object[] values, long wide, double real) throws Throwable {
        if (route == 0) return variant == 0 ? caller.raw(receiver, label, kinds, values, wide, real)
                                          : caller.narrow(receiver, label, kinds, values, wide, real);
        if (route == 1) return variant == 0 ? PrivateResultCaller.raw(caller, receiver, label, kinds, values, wide, real)
                                          : PrivateResultCaller.narrow(caller, receiver, label, kinds, values, wide, real);
        String name = variant == 0 ? "raw" : "narrow";
        Class<?>[] parameters = {Owner.Worker.class, String.class, Class[].class, Object[].class, long.class, double.class};
        if (route == 2) {
            try { return Owner.Worker.class.getMethod(name, parameters).invoke(caller, receiver, label, kinds, values, wide, real); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        MethodHandle method = MethodHandles.publicLookup().findVirtual(Owner.Worker.class, name,
            MethodType.methodType(variant == 0 ? Object.class : String.class, parameters));
        if (variant == 0) return (Object) method.invokeExact(caller, receiver, label, kinds, values, wide, real);
        return (String) method.invokeExact(caller, receiver, label, kinds, values, wide, real);
    }
    public static void main(String[] args) throws Throwable {
        int calls = 0, failures = 0;
        Owner first = new Owner(), second = new Owner(); first.token = new String("owned other");
        Owner.Worker caller = first.new Worker(), concrete = new PrivateResultDerived(second);
        int[] mutable = {0};
        for (int variant = 0; variant < 2; variant++) for (int route = 0; route < 4; route++)
        for (int absent = 0; absent < 2; absent++) for (int mode = 0; mode < 9; mode++) for (int attempt = 0; attempt < 2; attempt++) {
            calls++; mutable[0]++;
            Owner.Worker receiver = absent == 0 ? concrete : null;
            String label = attempt == 0 ? new String("owned label") : null;
            second.token = attempt == 0 ? new String("owned fallback") : null;
            Class<?>[] kinds = attempt == 0 ? new Class<?>[]{String.class, int.class} : null;
            Object selected = mode == 1 ? null : mode == 2 ? Integer.valueOf(7123) : new String("owned value");
            Object[] values = mode == 3 ? null : mode == 4 ? new Object[0] : new Object[]{selected, mutable};
            long wide = attempt == 0 ? 0x12345678abcdef01L : Long.MIN_VALUE + 19;
            long realBits = attempt == 0 ? 0x7ff8000000001234L : 0x8000000000000000L;
            double real = Double.longBitsToDouble(realBits);
            Trace.events = 0; Trace.failAt = mode >= 5 ? mode - 4 : 0;
            Trace.failure = new IllegalArgumentException("owned private failure");
            Trace.receiver = null; Trace.owner = null; Trace.label = null; Trace.kinds = null; Trace.values = null;
            Trace.providerKinds = null; Trace.providerValues = null; Trace.wide = 0; Trace.real = 0;
            Object result = null; Throwable thrown = null;
            try { result = call(variant, route, caller, receiver, label, kinds, values, wide, real); }
            catch (Throwable failure) { thrown = failure; }
            int events = absent != 0 ? 0 : mode == 5 ? 1 : mode == 6 ? 12 : mode == 7 || mode == 3 || mode == 4 ? 123 : 1234;
            require(Trace.events == events);
            if (absent != 0 || mode == 3) require(thrown instanceof NullPointerException);
            else if (mode == 4) require(thrown instanceof ArrayIndexOutOfBoundsException);
            else if (mode >= 5) require(thrown == Trace.failure);
            else if (mode == 2 && variant == 1) require(thrown instanceof ClassCastException);
            else require(thrown == null && result == (selected == null ? second.token : selected));
            if (thrown != null) failures++;
            boolean saved = absent == 0 && mode != 5;
            if (saved) require(Trace.receiver == concrete && Trace.owner == second && Trace.label == label
                && Trace.kinds == kinds && Trace.values == values && Trace.wide == wide && Double.doubleToRawLongBits(Trace.real) == realBits);
            else require(Trace.receiver == null && Trace.owner == null && Trace.label == null && Trace.kinds == null && Trace.values == null);
            if (absent == 0 && mode != 5 && mode != 6 && mode != 7)
                require(Trace.providerKinds == kinds && Trace.providerValues == values);
            else require(Trace.providerKinds == null && Trace.providerValues == null);
        }
        require(calls == 288 && failures == 248 && mutable[0] == calls);
        System.out.println("calls=" + calls + ",failures=" + failures + ",private-dispatch-and-result-checks=matched");
    }
}
