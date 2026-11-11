package fixture;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class PackageBoundScalarHarness {
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned package bound scalar mismatch");
    }
    @SuppressWarnings("unchecked")
    static <E extends Throwable> void raise(Throwable failure) throws E { throw (E) failure; }
    public static void main(String[] args) throws Throwable {
        final int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[2]);
        final boolean future = Boolean.parseBoolean(args[1]);
        Method declaration = Provider.class.getDeclaredMethod("read", Bound.class);
        TypeVariable<?>[] formals = declaration.getTypeParameters();
        require(Provider.class.getModifiers() == Modifier.ABSTRACT && declaration.getModifiers() == Modifier.ABSTRACT);
        require(formals.length == 1 && formals[0].getGenericDeclaration().equals(declaration));
        require(formals[0].getBounds().length == 1 && formals[0].getBounds()[0] == Bound.class);
        require(declaration.getGenericParameterTypes()[0].equals(formals[0]) && declaration.getGenericReturnType() == int.class);
        require(declaration.getExceptionTypes().length == 0 && Provider.class.getTypeParameters().length == 0);
        TypeVariable<?> ownerFormal = Host.class.getTypeParameters()[0];
        Method calculation = Host.class.getDeclaredMethod("calculate", int.class, Bound.class);
        require(calculation.getGenericParameterTypes()[1].equals(ownerFormal) && !ownerFormal.equals(formals[0]));
        Method method = Core.class.getMethod("read", Bound.class);
        MethodHandle handle = MethodHandles.lookup().findStatic(Core.class, "read", MethodType.methodType(int.class, Bound.class));
        for (int attempt = 0; attempt < 2; ++attempt) {
            final Bound value = mode == 1 ? null : new Bound();
            final int original = mode == 9 ? attempt == 0 ? Integer.MIN_VALUE : Integer.MAX_VALUE : 17 + attempt;
            if (value != null) value.number = original;
            final int base = attempt == 0 ? Integer.MAX_VALUE : Integer.MIN_VALUE;
            final Provider first = mode == 2 || mode == 4 ? null : future ? new PackageBoundScalarFuture() : new Impl();
            final Provider second = future ? new PackageBoundScalarFuture() : new Impl();
            final Host<Bound> host = new Host<Bound>(first);
            host.selector = mode == 10 ? 1 : mode == 11 ? 2 : 0;
            final Throwable selected = mode == 4 || mode == 7 ? new java.io.IOException()
                    : mode == 5 || mode == 8 ? new AssertionError() : new IllegalStateException();
            final int[] counts = {0, 0};
            Trace.events = 0; Trace.current = first; Trace.replacement = second;
            Trace.receiver = null; Trace.argument = null; Trace.observed = 0; Trace.finished = 0;
            Trace.inputAction = new Runnable() { public void run() {
                require(++counts[0] == 1 && counts[1] == 0 && Trace.events == 12);
                require(Trace.current == second && Trace.receiver == null && Trace.argument == null);
                if (mode >= 3 && mode <= 5) PackageBoundScalarHarness.<RuntimeException>raise(selected);
            }};
            Trace.action = new Runnable() { public void run() {
                require(++counts[1] == 1 && counts[0] == 1);
                require(Trace.receiver == first && Trace.argument == value && Trace.observed == original);
                require(Trace.current == second && value.number == original);
                value.number = 91;
                if (mode >= 6 && mode <= 8) PackageBoundScalarHarness.<RuntimeException>raise(selected);
            }};
            int result = 0; Throwable failure = null;
            try {
                if (route == 0) result = Core.read(value);
                else if (route == 1) {
                    try { result = (Integer) method.invoke(null, value); }
                    catch (InvocationTargetException caught) { throw caught.getCause(); }
                } else if (route == 2) result = (int) handle.invokeExact(value);
                else if (route == 3) result = Core.nested(value);
                else if (route == 4) result = PackageBoundScalarCaller.read(value);
                else result = host.run(base, value);
            } catch (Throwable caught) { failure = caught; }
            boolean bypass = route == 5 && mode >= 10;
            boolean entered = !bypass && mode != 2 && !(mode >= 3 && mode <= 5);
            boolean read = entered && mode != 1;
            boolean completed = read && !(mode >= 6 && mode <= 8);
            String sequence = "1";
            if (!bypass) sequence += "2";
            if (entered) sequence += (future ? "9" : "") + "3";
            if (read) sequence += "4";
            if (completed) sequence += "58";
            if (bypass && mode == 10) sequence += "8";
            require(Trace.events == Long.parseLong(sequence));
            require(counts[0] == (bypass ? 0 : 1) && counts[1] == (read ? 1 : 0));
            require(Trace.current == (bypass ? first : second));
            require(Trace.receiver == (entered ? first : null));
            require(Trace.argument == (entered ? value : null));
            require(Trace.observed == (read ? original : 0));
            if (read) require(value.number == 91);
            int returned = future ? original ^ 0x40000000 : original;
            require(Trace.finished == (completed && route == 3 ? returned : 0));
            if (bypass && mode == 11) require(failure instanceof IllegalStateException);
            else if (bypass) require(failure == null && result == base);
            else if (mode == 1 || mode == 2) require(failure instanceof NullPointerException);
            else if (mode >= 3 && mode <= 8) require(failure == selected);
            else require(failure == null && result == (route == 5 ? base - returned : returned));
        }
        System.out.println("attempts=2,package-bound-scalar=matched");
    }
}
