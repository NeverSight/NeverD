import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class IntersectionConstructorWitnessHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned intersection constructor witness mismatch");
    }
    @SuppressWarnings("unchecked")
    static <E extends Throwable> void raise(Throwable failure) throws E { throw (E) failure; }
    public static void main(String[] args) throws Throwable {
        final int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[2]);
        final boolean future = Boolean.parseBoolean(args[1]), wrong = Boolean.parseBoolean(args[3]);
        Constructor<?> declaration = Product.class.getConstructor(Bound.class);
        TypeVariable<?>[] variables = declaration.getTypeParameters();
        require(variables.length == 1 && variables[0].getGenericDeclaration().equals(declaration));
        require(variables[0].getBounds().length == 2 && variables[0].getBounds()[0] == Bound.class
                && variables[0].getBounds()[1] == Flag.class);
        require(declaration.getGenericParameterTypes()[0].equals(variables[0]) && declaration.getExceptionTypes().length == 0);
        require(Product.class.getDeclaredConstructors().length == 2 && Product.class.getTypeParameters().length == 0);
        Method method = Core.class.getMethod("make", Value.class);
        MethodHandle handle = MethodHandles.lookup().findStatic(Core.class, "make",
                MethodType.methodType(Product.class, Value.class));
        for (int attempt = 0; attempt < 2; ++attempt) {
            final Value value = mode == 1 ? null : future ? new Value() {
                public void touch() { Trace.hit(9); super.touch(); }
            } : new Value();
            if (value != null) value.number = 17 + attempt;
            final Throwable runtime = new IllegalStateException(), checked = new java.io.IOException(), error = new AssertionError();
            final Throwable selected = mode == 4 || mode == 8 || mode == 10 ? checked
                    : mode == 5 || mode == 9 || mode == 11 ? error : runtime;
            final int[] counts = {0, 0, 0};
            Trace.events = 0; Trace.product = null; Trace.value = null; Trace.flag = null; Trace.observed = 0;
            Trace.initAction = new Runnable() { public void run() {
                require(++counts[0] == 1 && Trace.events == 12 && Trace.product == null);
                if (mode >= 10) IntersectionConstructorWitnessHarness.<RuntimeException>raise(selected);
            }};
            Trace.touchAction = mode == 2 ? null : new Runnable() { public void run() {
                require(++counts[1] == 1 && Trace.product != null && Trace.value != null && Trace.flag == Trace.value);
                require(Trace.observed == (route == 5 ? 0 : value.number));
                if (mode >= 3 && mode <= 5) IntersectionConstructorWitnessHarness.<RuntimeException>raise(selected);
                Trace.value.number = 91;
            }};
            Trace.action = mode == 6 ? null : new Runnable() { public void run() {
                require(++counts[2] == 1 && counts[1] == 1 && Trace.value.number == 91);
                if (mode >= 7 && mode <= 9) IntersectionConstructorWitnessHarness.<RuntimeException>raise(selected);
            }};
            Product result = null; Core core = null; Throwable failure = null;
            try {
                if (route == 0) result = Core.make(value);
                else if (route == 1) {
                    try { result = (Product) method.invoke(null, value); }
                    catch (InvocationTargetException caught) { throw caught.getCause(); }
                } else if (route == 2) result = (Product) handle.invokeExact(value);
                else if (route == 3) result = Core.nested(value);
                else if (route == 4) result = IntersectionConstructorWitnessCaller.call(value);
                else { core = new Core(); result = core.result; }
            } catch (Throwable caught) { failure = caught; }
            boolean initFailure = mode >= 10;
            boolean missing = mode == 1 && route != 5;
            boolean entered = !initFailure && !wrong;
            boolean touched = entered && !missing;
            boolean touchSucceeded = touched && !(mode >= 2 && mode <= 5);
            boolean actionSucceeded = touchSucceeded && !(mode >= 6 && mode <= 9);
            String sequence = "1" + (attempt == 0 ? "2" : "");
            if (entered) sequence += "3";
            if (touched) sequence += "4" + (future && route != 5 ? "9" : "") + "5";
            if (touchSucceeded) sequence += "6";
            if (actionSucceeded) sequence += "78";
            require(Trace.events == Long.parseLong(sequence));
            require(counts[0] == (attempt == 0 ? 1 : 0));
            require(counts[1] == (touched && mode != 2 ? 1 : 0));
            require(counts[2] == (touchSucceeded && mode != 6 ? 1 : 0));
            require((Trace.product != null) == entered);
            require(Trace.value == (entered ? route == 5 ? Trace.value : value : null));
            if (entered && route == 5) require(Trace.value instanceof Core);
            require(Trace.flag == Trace.value);
            require(Trace.observed == (touched ? route == 5 ? 0 : 17 + attempt : 0));
            require(result == (actionSucceeded ? Trace.product : null));
            if (initFailure && attempt == 1) require(failure instanceof NoClassDefFoundError);
            else if (initFailure && mode == 10) require(failure instanceof ExceptionInInitializerError && failure.getCause() == selected);
            else if (initFailure) require(failure == selected);
            else if (wrong) require(failure instanceof ClassCastException);
            else if (missing || mode == 2 || mode == 6) require(failure instanceof NullPointerException);
            else if (mode >= 3 && mode <= 5 || mode >= 7 && mode <= 9) require(failure == selected);
            else require(failure == null);
            if (core != null) require(core == Trace.value && core.result == Trace.product);
        }
        System.out.println("attempts=2,intersection-constructor-witness=matched");
    }
}
