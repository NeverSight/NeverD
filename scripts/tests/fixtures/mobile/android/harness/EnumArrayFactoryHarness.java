import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.concurrent.TimeUnit;

public final class EnumArrayFactoryHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("enum array factory changed"); }
    static void selfBound(TypeVariable<?> formal) {
        require(formal.getBounds().length == 1);
        ParameterizedType bound = (ParameterizedType) formal.getBounds()[0];
        require(bound.getRawType() == Enum.class && bound.getActualTypeArguments().length == 1
            && bound.getActualTypeArguments()[0].equals(formal));
    }
    static Object call(int route, boolean checked, Enum[] values) throws Throwable {
        if (route == 0) return checked ? Client.checked(values) : Client.call(values);
        if (route == 1) return checked ? EnumArrayFactoryCaller.checked(values) : EnumArrayFactoryCaller.call(values);
        String name = checked ? "checked" : "call";
        if (route == 2) {
            try { return Client.class.getMethod(name, Enum[].class).invoke(null, (Object) values); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (checked) return (Thread.State) MethodHandles.publicLookup().findStatic(Client.class, name,
            MethodType.methodType(Thread.State.class, Enum[].class)).invokeExact(values);
        return (Object) MethodHandles.publicLookup().findStatic(Client.class, name,
            MethodType.methodType(Object.class, Enum[].class)).invokeExact(values);
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        boolean checked = operation == 1;
        Class<?> owner = Class.forName("fixture.api.Provider", false, Client.class.getClassLoader());
        Method method = owner.getDeclaredMethod("make", Enum[].class);
        TypeVariable<?>[] formals = method.getTypeParameters();
        require(Modifier.isStatic(method.getModifiers()) && formals.length == 1);
        selfBound(formals[0]);
        require(Box.class.getTypeParameters().length == 1);
        selfBound(Box.class.getTypeParameters()[0]);
        selfBound(Enum.class.getTypeParameters()[0]);
        Type[] parameters = method.getGenericParameterTypes();
        require(parameters.length == 1 && parameters[0] instanceof GenericArrayType
            && ((GenericArrayType) parameters[0]).getGenericComponentType().equals(formals[0]));
        require(method.getReturnType() == Box.class
            && ((ParameterizedType) method.getGenericReturnType()).getActualTypeArguments()[0].equals(formals[0]));
        Trace.failAt = mode >= 1 && mode <= 4 ? mode : mode == 14 ? 5 : 0;
        Trace.failure = new RuntimeException("owned enum array failure");
        Enum old = Thread.State.NEW;
        Enum[] values = mode == 5 ? null : mode == 6 || mode == 18 ? new Enum[0]
            : mode == 19 ? new Thread.State[0]
            : mode == 7 || mode == 8 || mode == 16 ? new Thread.State[]{(Thread.State) old} : new Enum[]{old};
        Trace.replacement = mode == 7 || mode == 19 ? TimeUnit.DAYS : mode == 17 || mode == 18 ? new Object()
            : mode == 16 ? null : Thread.State.RUNNABLE;
        Enum element = mode == 11 ? TimeUnit.SECONDS : mode == 12 ? TimeUnit.DAYS
            : mode == 15 ? (Enum) Trace.replacement : mode == 16 ? null : Thread.State.WAITING;
        Box box = new Box();
        box.value = element; box.payload = mode == 13 ? values : new Object();
        Trace.result = mode == 9 ? new Object() : mode == 10 ? null : box;
        Object sentinel = new Object();
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.after = sentinel; Trace.seen = sentinel; Trace.length = -1;
            Object before = values != null && values.length != 0 ? values[0] : null;
            Throwable failure = null; Object result = null;
            try { result = call(route, checked, values); } catch (Throwable problem) { failure = problem; }
            int trace = attempt == 0 ? 1 : 0;
            boolean body = mode != 1;
            if (body) trace = trace * 10 + 2;
            boolean length = body && mode != 2 && mode != 5;
            if (length) trace = trace * 10 + 3;
            boolean stored = length && mode != 3 && mode != 6 && mode != 7
                && mode != 17 && mode != 18 && mode != 19;
            if (stored) trace = trace * 10 + 4;
            boolean returned = stored && mode != 4 && mode != 9;
            if (returned) trace = trace * 10 + 5;
            require(Trace.events == trace && Trace.seen == (body ? values : sentinel)
                && Trace.length == (length ? values.length : -1));
            if (values != null && values.length != 0) require(values[0] == (stored ? Trace.replacement : before));
            require(Trace.after == (returned ? Trace.result : sentinel));
            if (mode == 1) {
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (mode == 2 || mode == 3 || mode == 4 || mode == 14) require(failure == Trace.failure);
            else if (mode == 5 || (checked && mode == 10)) require(failure instanceof NullPointerException);
            else if (mode == 6 || mode == 19) require(failure instanceof ArrayIndexOutOfBoundsException);
            else if (mode == 7) require(failure instanceof ArrayStoreException);
            else if (mode == 9 || mode == 17 || mode == 18 || (checked && (mode == 11 || mode == 12)))
                require(failure instanceof ClassCastException);
            else {
                require(failure == null && result == (checked ? element : Trace.result));
                if (!checked && mode == 13) require(((Box) result).payload == values);
                if (!checked && mode == 15) require(((Box) result).value == values[0]);
                if (!checked && mode == 0) {
                    ((Box) result).payload = sentinel;
                    require(((Box) Trace.result).payload == sentinel);
                }
            }
            System.out.println("route=" + route + ",operation=" + operation + ",mode=" + mode
                + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
