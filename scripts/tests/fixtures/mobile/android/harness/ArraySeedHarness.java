import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.*;

public final class ArraySeedHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("array seed factory changed"); }
    static Object call(int route, boolean checked, Object[] values) throws Throwable {
        if (route == 0) return checked ? Client.checked(values) : Client.call(values);
        if (route == 1) return checked ? ArraySeedCaller.checked(values) : ArraySeedCaller.call(values);
        String name = checked ? "checked" : "call";
        if (route == 2) {
            try { return Client.class.getMethod(name, Object[].class).invoke(null, (Object) values); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (checked) return (String) MethodHandles.publicLookup().findStatic(Client.class, name,
            MethodType.methodType(String.class, Object[].class)).invokeExact(values);
        return (Object) MethodHandles.publicLookup().findStatic(Client.class, name,
            MethodType.methodType(Object.class, Object[].class)).invokeExact(values);
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        boolean checked = operation == 1;
        Class<?> owner = Class.forName("fixture.api.Provider", false, Client.class.getClassLoader());
        Method method = owner.getDeclaredMethod("make", Object[].class);
        TypeVariable<?>[] formals = method.getTypeParameters();
        require(Modifier.isStatic(method.getModifiers()) && formals.length == 1
            && formals[0].getBounds().length == 1 && formals[0].getBounds()[0] == Object.class);
        Type[] parameters = method.getGenericParameterTypes();
        require(parameters.length == 1 && parameters[0] instanceof GenericArrayType
            && ((GenericArrayType) parameters[0]).getGenericComponentType().equals(formals[0]));
        require(method.getReturnType() == List.class
            && ((ParameterizedType) method.getGenericReturnType()).getActualTypeArguments()[0].equals(formals[0]));
        Trace.failAt = mode >= 1 && mode <= 4 ? mode : 0;
        Trace.failure = new RuntimeException("owned array seed failure");
        Object old = new String("owned old element");
        Object[] values = mode == 5 ? null : mode == 6 ? new Object[0]
            : mode == 7 || mode == 8 || mode == 16 ? new String[]{(String) old} : new Object[]{old};
        Trace.replacement = mode == 7 ? new Object() : mode == 16 ? null : new String("owned replacement");
        Object element = mode == 12 ? new Object() : mode == 13 ? values
            : mode == 15 ? Trace.replacement : mode == 16 ? null : new String("owned result");
        List list = new ArrayList();
        if (mode != 11) list.add(element);
        Trace.result = mode == 9 ? new Object() : mode == 10 ? null
            : mode == 14 ? Collections.singletonList(element) : list;
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
            boolean stored = length && mode != 3 && mode != 6 && mode != 7;
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
            } else if (mode == 2 || mode == 3 || mode == 4) require(failure == Trace.failure);
            else if (mode == 5 || (checked && mode == 10)) require(failure instanceof NullPointerException);
            else if (mode == 6) require(failure instanceof ArrayIndexOutOfBoundsException);
            else if (mode == 7) require(failure instanceof ArrayStoreException);
            else if (mode == 9 || (checked && (mode == 12 || mode == 13))) require(failure instanceof ClassCastException);
            else if (checked && mode == 11) require(failure instanceof IndexOutOfBoundsException);
            else {
                require(failure == null && result == (checked ? element : Trace.result));
                if (!checked && mode == 13) require(((List) result).get(0) == values);
                if (!checked && mode == 15) require(((List) result).get(0) == values[0]);
                if (!checked && mode == 0) {
                    ((List) result).add(sentinel);
                    require(((List) Trace.result).get(((List) Trace.result).size() - 1) == sentinel);
                }
                if (!checked && mode == 14) {
                    try { ((List) result).add(sentinel); throw new AssertionError("immutable result changed"); }
                    catch (UnsupportedOperationException expected) { }
                }
            }
            System.out.println("route=" + route + ",operation=" + operation + ",mode=" + mode
                + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
