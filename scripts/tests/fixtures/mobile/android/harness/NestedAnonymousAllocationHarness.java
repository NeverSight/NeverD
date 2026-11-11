package fixture;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class NestedAnonymousAllocationHarness {
    public static final class FutureContext extends Context { }
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned nested anonymous allocation mismatch");
    }
    @SuppressWarnings("unchecked")
    static <E extends Throwable> void raise(Throwable failure) throws E { throw (E) failure; }
    static void identity(Callback callback) throws Exception {
        Class<?> type = callback.getClass();
        require(type.getName().equals("fixture.Owner$1") && type.isAnonymousClass() && !type.isLocalClass());
        require(type.getEnclosingClass() == Owner.class && type.getEnclosingMethod() == null && type.getEnclosingConstructor() == null);
        require(type.getSuperclass() == Object.class && type.getDeclaredFields().length == 0);
        require(java.util.Arrays.equals(type.getInterfaces(), new Class<?>[]{Callback.class}));
        require(type.getModifiers() == 0 && type.getTypeParameters().length == 0);
        Constructor<?>[] constructors = type.getDeclaredConstructors();
        require(constructors.length == 1 && constructors[0].getParameterTypes().length == 0 && constructors[0].getModifiers() == 0);
        Method call = type.getDeclaredMethod("call", Object.class);
        require(call.getReturnType() == boolean.class && call.getModifiers() == Modifier.PUBLIC);
        require(call.getTypeParameters().length == 0 && call.getExceptionTypes().length == 0);
        int originals = 0;
        for (Method method : type.getDeclaredMethods()) {
            if (method.getName().equals("__neverdThrow")) {
                require(method.getModifiers() == Modifier.PRIVATE && method.getReturnType() == RuntimeException.class);
                require(java.util.Arrays.equals(method.getParameterTypes(), new Class<?>[]{Throwable.class}));
                require(method.getTypeParameters().length == 1);
                TypeVariable<?> variable = method.getTypeParameters()[0];
                require(java.util.Arrays.equals(variable.getBounds(), new Type[]{Throwable.class}));
                require(java.util.Arrays.equals(method.getGenericExceptionTypes(), new Type[]{variable}));
            } else { require(method.equals(call)); ++originals; }
        }
        require(originals == 1);
    }
    public static void main(String[] args) throws Throwable {
        final int route = Integer.parseInt(args[0]), kind = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        final Field field = Owner.class.getField("value");
        final MethodHandle getter = MethodHandles.lookup().findStaticGetter(Owner.class, "value", Target.class);
        TypeVariable<?>[] formals = Owner.class.getTypeParameters();
        require(formals.length == 1 && Owner.class.getModifiers() == (Modifier.PUBLIC | Modifier.ABSTRACT));
        ParameterizedType bound = (ParameterizedType) formals[0].getBounds()[0];
        require(bound.getRawType() == Owner.class && bound.getActualTypeArguments()[0].equals(formals[0]));
        final Context initial = kind == 2 ? null : kind == 1 ? new FutureContext() : new Context();
        final Context replacement = new FutureContext();
        final Object input = kind == 1 ? null : new Object(), changedInput = new Object();
        final Throwable selected = mode == 2 || mode == 5 || mode == 8 || mode == 11 ? new java.io.IOException()
                : mode == 3 || mode == 6 || mode == 9 || mode == 12 ? new AssertionError() : new IllegalStateException();
        final int[] counts = {0, 0, 0, 0};
        Trace.events = 0; Trace.current = initial; Trace.input = input; Trace.receiver = null; Trace.argument = null;
        Trace.constructed = null; Trace.flag = kind == 1; Trace.keep = mode != 20;
        Trace.initializer = mode == 13 ? null : new Runnable() { public void run() {
            require(++counts[0] == 1 && Trace.events == 12 && Trace.constructed == null);
            if (mode >= 1 && mode <= 3) NestedAnonymousAllocationHarness.<RuntimeException>raise(selected);
        }};
        Trace.contextAction = mode == 14 ? null : new Runnable() { public void run() {
            require(++counts[1] == 1 && counts[0] == 1 && Trace.events == 123 && Trace.constructed == null);
            if (mode >= 4 && mode <= 6) NestedAnonymousAllocationHarness.<RuntimeException>raise(selected);
            if (mode == 18) Trace.current = replacement;
        }};
        Trace.constructorAction = mode == 15 ? null : new Runnable() { public void run() {
            require(++counts[2] == 1 && counts[1] == 1 && Trace.events == 1234);
            require(Trace.constructed != null && (Trace.constructed.callback != null) == (mode != 20));
            require(Trace.constructed.context == (mode == 18 ? replacement : initial));
            if (mode >= 7 && mode <= 9) NestedAnonymousAllocationHarness.<RuntimeException>raise(selected);
            if (mode == 17) { Trace.input = changedInput; Trace.flag = !Trace.flag; }
        }};
        Trace.action = mode == 16 ? null : new Runnable() { public void run() {
            require(++counts[3] == 1 && counts[2] == 1 && Trace.events == 12345);
            require(Trace.receiver == Trace.constructed.callback && Trace.argument == (mode == 17 ? changedInput : input));
            if (mode >= 10 && mode <= 12) NestedAnonymousAllocationHarness.<RuntimeException>raise(selected);
            if (mode == 19) Trace.flag = !Trace.flag;
        }};
        final int stop = mode >= 1 && mode <= 3 || mode == 13 ? 0
                : mode >= 4 && mode <= 6 || mode == 14 ? 1
                : mode >= 7 && mode <= 9 || mode == 15 || mode == 20 ? 2
                : mode >= 10 && mode <= 12 || mode == 16 ? 3 : 4;
        long expected = new long[]{12, 123, 1234, 12345, 12345679}[stop];
        Target first = null;
        for (int attempt = 0; attempt < 2; ++attempt) {
            Target value = null; Throwable failure = null;
            try {
                if (route == 0) value = Owner.value;
                else if (route == 1) value = (Target) field.get(null);
                else value = (Target) getter.invokeExact();
            } catch (Throwable caught) { failure = caught; }
            require(Trace.events == expected);
            for (int stage = 0; stage < 4; ++stage)
                require(counts[stage] == (stage <= stop && mode != 13 + stage ? 1 : 0));
            boolean constructed = stop >= 2, entered = stop >= 3;
            require((Trace.constructed != null) == constructed);
            if (constructed) {
                require(Trace.constructed.context == (mode == 18 ? replacement : initial));
                require((Trace.constructed.callback != null) == (mode != 20));
                if (Trace.constructed.callback != null) identity(Trace.constructed.callback);
            }
            require(Trace.receiver == (entered ? Trace.constructed.callback : null));
            require(Trace.argument == (entered ? (mode == 17 ? changedInput : input) : null));
            if (stop == 4) {
                require(failure == null && value == Trace.constructed);
                require(value.result == ((kind == 1) ^ (mode == 17 || mode == 19)));
                if (attempt == 0) first = value; else require(value == first);
            } else if (attempt == 1) require(failure instanceof NoClassDefFoundError);
            else if (mode >= 13 && mode <= 16 || mode == 20)
                require(failure instanceof ExceptionInInitializerError && failure.getCause() instanceof NullPointerException);
            else if (selected instanceof Error) require(failure == selected);
            else require(failure instanceof ExceptionInInitializerError && failure.getCause() == selected);
        }
        // The anonymous object remains callable after partial outer construction
        // and failed enclosing initialization. It retains one original identity.
        if (Trace.constructed != null && Trace.constructed.callback != null) {
            final Callback callback = Trace.constructed.callback;
            Method call = callback.getClass().getDeclaredMethod("call", Object.class);
            MethodHandle handle = MethodHandles.lookup().findVirtual(Callback.class, "call",
                    MethodType.methodType(boolean.class, Object.class));
            for (int attempt = 0; attempt < 2; ++attempt) {
                final Object argument = attempt == 0 ? new Object() : null;
                final Throwable failure = new java.io.IOException();
                final int[] calls = {0};
                Trace.events = 0; Trace.receiver = null; Trace.argument = null; Trace.flag = attempt == 0;
                Trace.action = new Runnable() { public void run() {
                    require(++calls[0] == 1 && Trace.events == 5 && Trace.receiver == callback && Trace.argument == argument);
                    if (argument == null) NestedAnonymousAllocationHarness.<RuntimeException>raise(failure);
                }};
                Throwable caught = null; boolean value = false;
                try {
                    if (route == 0) value = callback.call(argument);
                    else if (route == 1) {
                        try { value = (Boolean) call.invoke(callback, argument); }
                        catch (InvocationTargetException wrapped) { throw wrapped.getCause(); }
                    } else value = (boolean) handle.invokeExact(callback, argument);
                } catch (Throwable error) { caught = error; }
                require(calls[0] == 1 && Trace.events == (argument == null ? 5 : 56));
                require(argument == null ? caught == failure : caught == null && value);
                identity(callback);
            }
        }
        System.out.println("attempts=2,nested-anonymous-allocation=matched");
    }
}
