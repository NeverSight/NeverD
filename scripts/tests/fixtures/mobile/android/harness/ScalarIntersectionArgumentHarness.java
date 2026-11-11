import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ScalarIntersectionArgumentHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned scalar intersection argument invariant");
    }
    @SuppressWarnings("unchecked")
    static <E extends Throwable> void raise(Throwable failure) throws E { throw (E)failure; }
    public static void main(String[] args) throws Throwable {
        final int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[2]);
        final boolean future = Boolean.parseBoolean(args[1]), wrong = Boolean.parseBoolean(args[3]);
        final Method method = Core.class.getMethod("run", long.class, int.class, boolean.class, double.class);
        final MethodHandle handle = MethodHandles.lookup().findVirtual(Core.class, "run",
            MethodType.methodType(void.class, long.class, int.class, boolean.class, double.class));
        Method declaration = Provider.class.getMethod("accept", long.class, int.class, Bound.class,
            boolean.class, double.class);
        TypeVariable<?> formal = declaration.getTypeParameters()[0];
        require(declaration.getTypeParameters().length == 1 && formal.getBounds().length == 2);
        require(formal.getBounds()[0] == Bound.class && formal.getBounds()[1] == Flag.class);
        require(declaration.getGenericParameterTypes()[2].equals(formal) && declaration.getExceptionTypes().length == 0);
        for (int attempt = 0; attempt < 2; ++attempt) {
            final Core core = new Core();
            final Child provider = future ? new ScalarIntersectionArgumentFuture() : new Child();
            final Value value = mode == 1 ? null : new Value(42 + attempt);
            final Child replacement = new Child();
            final Value replacementValue = new Value(-1);
            core.provider = mode == 2 ? null : provider;
            core.value = value;
            final long number = 0xfedcba9876543210L ^ ((long)mode << 32) ^ attempt;
            final int label = Integer.MIN_VALUE + mode + attempt;
            final boolean enabled = (mode + attempt) % 2 == 0;
            final long bits = mode == 14 ? 0x7ff8123456789abcL : attempt == 0 ? 0x8000000000000000L : 0x400921fb54442d18L;
            final double fraction = Double.longBitsToDouble(bits);
            final RuntimeException runtime = new IllegalStateException();
            final Error error = new AssertionError();
            final java.io.IOException checked = new java.io.IOException();
            final Throwable hitFailure = mode == 9 || mode == 11 ? checked : mode == 10 || mode == 12 ? error : runtime;
            Trace.events = 0; Trace.number = 0; Trace.label = 0; Trace.enabled = false;
            Trace.fraction = 0; Trace.seenTag = 0; Trace.receiver = null; Trace.value = null;
            Trace.failAt = mode == 7 ? 4 : mode == 8 ? 5 : mode == 9 ? 7 : mode == 10 ? 1 : mode == 11 ? 9 : mode == 12 ? 6 : -1;
            Trace.failure = new Runnable() { public void run() { ScalarIntersectionArgumentHarness.<RuntimeException>raise(hitFailure); } };
            final int[] calls = {0};
            Trace.action = mode == 3 ? null : new Runnable() {
                public void run() {
                    require(++calls[0] == 1 && Trace.receiver == provider && Trace.value == value);
                    require(Trace.number == number && Trace.label == label && Trace.enabled == enabled);
                    require(Double.doubleToRawLongBits(Trace.fraction) == bits && Trace.seenTag == 0);
                    if (mode == 4) throw runtime;
                    if (mode == 5) throw error;
                    if (mode == 6) ScalarIntersectionArgumentHarness.<RuntimeException>raise(checked);
                    core.provider = replacement; core.value = replacementValue;
                    if (mode == 15) { Trace.number = 99; value.tag = 73; }
                }
            };
            Throwable failure = null;
            try {
                if (route == 0) core.run(number, label, enabled, fraction);
                else if (route == 1) ScalarIntersectionArgumentCaller.call(core, number, label, enabled, fraction);
                else if (route == 2) {
                    try { method.invoke(core, number, label, enabled, fraction); }
                    catch (InvocationTargetException caught) { throw caught.getCause(); }
                } else handle.invokeExact(core, number, label, enabled, fraction);
            } catch (Throwable caught) { failure = caught; }
            String base = wrong ? "456" : mode == 7 ? "4" : mode >= 3 && mode <= 6 || mode == 8 ? "45"
                : mode == 12 ? "456" : mode == 9 ? "4567" : value == null ? "4568" : "45678";
            if (mode == 2 || future && mode == 10) base = "";
            boolean completed = base.endsWith("8");
            String sequence = mode == 2 ? "" : (future ? "1" : "") + base + (future && completed ? "9" : "");
            long events = sequence.isEmpty() ? 0 : Long.parseLong(sequence);
            require(Trace.events == events);
            boolean stored = base.startsWith("45");
            require(Trace.receiver == (stored ? provider : null) && Trace.value == (stored ? value : null));
            require(Trace.number == (stored ? mode == 15 ? 99 : number : 0));
            require(Trace.label == (stored ? label : 0) && Trace.enabled == (stored && enabled));
            require(Double.doubleToRawLongBits(Trace.fraction) == (stored ? bits : 0));
            boolean invoked = stored && mode != 8 && mode != 3;
            require(calls[0] == (invoked ? 1 : 0));
            boolean changed = invoked && !(mode >= 4 && mode <= 6);
            require(core.provider == (changed ? replacement : mode == 2 ? null : provider));
            require(core.value == (changed ? replacementValue : value));
            require(Trace.seenTag == (completed && value != null ? mode == 15 ? 73 : 42 + attempt : 0));
            if (wrong) require(failure instanceof ClassCastException);
            else if (mode == 2 || mode == 3) require(failure instanceof NullPointerException);
            else if (mode == 4) require(failure == runtime);
            else if (mode == 5) require(failure == error);
            else if (mode == 6) require(failure == checked);
            else if (mode == 7 || mode == 8 || mode == 9 || mode == 12 || future && (mode == 10 || mode == 11)) require(failure == hitFailure);
            else require(failure == null);
            System.out.println("route=" + route + ",future=" + future + ",mode=" + mode + ",attempt=" + attempt + ",events=" + events);
        }
    }
}
