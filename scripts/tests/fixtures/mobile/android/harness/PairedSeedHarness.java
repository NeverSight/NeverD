import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.*;

public final class PairedSeedHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("paired seed factory changed");
    }
    static Object call(int route, int operation, Object first, Object second) throws Throwable {
        if (route == 0) return operation == 0 ? Client.call(first, second)
            : operation == 1 ? Client.checkedKey(first, second) : Client.checkedValue(first, second);
        if (route == 1) return operation == 0 ? PairedSeedCaller.call(first, second)
            : operation == 1 ? PairedSeedCaller.checkedKey(first, second) : PairedSeedCaller.checkedValue(first, second);
        String name = operation == 0 ? "call" : operation == 1 ? "checkedKey" : "checkedValue";
        if (route == 2) {
            try { return Client.class.getMethod(name, Object.class, Object.class).invoke(null, first, second); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (operation != 0) return (String) MethodHandles.publicLookup().findStatic(Client.class, name,
            MethodType.methodType(String.class, Object.class, Object.class)).invokeExact(first, second);
        return (Object) MethodHandles.publicLookup().findStatic(Client.class, name,
            MethodType.methodType(Object.class, Object.class, Object.class)).invokeExact(first, second);
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        Class<?> owner = Class.forName("fixture.api.Provider", false, Client.class.getClassLoader());
        Method method = owner.getDeclaredMethod("make", Object.class, Object.class);
        TypeVariable<?>[] formals = method.getTypeParameters();
        require(Modifier.isStatic(method.getModifiers()) && formals.length == 2);
        for (TypeVariable<?> formal : formals) require(formal.getBounds().length == 1
            && formal.getBounds()[0] == Object.class && formal.getGenericDeclaration().equals(method));
        Type[] inputs = method.getGenericParameterTypes();
        Type[] results = ((ParameterizedType) method.getGenericReturnType()).getActualTypeArguments();
        require(method.getReturnType() == Map.class && inputs.length == 2 && results.length == 2
            && inputs[0].equals(formals[0]) && inputs[1].equals(formals[1])
            && results[0].equals(formals[0]) && results[1].equals(formals[1]));
        Trace.failAt = mode >= 1 && mode <= 4 ? mode : mode == 22 ? 5 : 0;
        Trace.failure = new RuntimeException("owned paired seed failure");
        int[] first = mode == 20 ? new int[0] : new int[]{17};
        int[] second = mode == 21 ? new int[0] : mode == 17 ? first : new int[]{31};
        Object firstSeed = mode == 5 ? null : mode == 6 ? new Object() : first;
        Object secondSeed = mode == 7 ? null : mode == 8 ? new Object() : second;
        Object key = mode == 12 ? new Object() : mode == 15 ? first
            : mode == 18 ? null : new String("owned key");
        Object value = mode == 13 ? new Object() : mode == 16 ? second
            : mode == 19 ? key : mode == 18 ? null : new String("owned value");
        Map map = new HashMap();
        if (mode != 11) map.put(key, value);
        Trace.key = key;
        Trace.result = mode == 9 ? new Object() : mode == 10 ? null
            : mode == 14 ? Collections.singletonMap(key, value) : map;
        Object sentinel = new Object();
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.after = sentinel; Trace.first = sentinel; Trace.second = sentinel;
            int beforeFirst = first.length == 0 ? 0 : first[0];
            int beforeSecond = second.length == 0 ? 0 : second[0];
            Throwable failure = null; Object result = null;
            try { result = call(route, operation, firstSeed, secondSeed); }
            catch (Throwable problem) { failure = problem; }
            int trace = attempt == 0 ? 1 : 0;
            boolean body = mode != 1;
            if (body) trace = trace * 10 + 2;
            boolean firstMutation = body && mode != 2 && mode != 5 && mode != 6 && mode != 20;
            if (firstMutation) trace = trace * 10 + 3;
            boolean secondMutation = firstMutation && mode != 3 && mode != 7 && mode != 8 && mode != 21;
            if (secondMutation) trace = trace * 10 + 4;
            boolean returned = secondMutation && mode != 4 && mode != 9;
            if (returned) trace = trace * 10 + 5;
            require(Trace.events == trace && Trace.first == (body ? firstSeed : sentinel)
                && Trace.second == (body ? secondSeed : sentinel));
            if (first.length != 0) require(first[0] == beforeFirst + (firstMutation ? 1 : 0)
                + (first == second && secondMutation ? 3 : 0));
            if (second.length != 0) require(second[0] == beforeSecond + (secondMutation ? 3 : 0)
                + (first == second && firstMutation ? 1 : 0));
            require(Trace.after == (returned ? Trace.result : sentinel));
            if (mode == 1) {
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (mode == 2 || mode == 3 || mode == 4 || mode == 22) require(failure == Trace.failure);
            else if (mode == 5 || mode == 7 || (operation != 0 && mode == 10)) require(failure instanceof NullPointerException);
            else if (mode == 6 || mode == 8 || mode == 9 || (operation == 1 && (mode == 12 || mode == 15))
                    || (operation == 2 && (mode == 13 || mode == 16))) require(failure instanceof ClassCastException);
            else if (mode == 20 || mode == 21) require(failure instanceof ArrayIndexOutOfBoundsException);
            else if (operation == 1 && mode == 11) require(failure instanceof NoSuchElementException);
            else {
                Object expected = operation == 0 ? Trace.result : operation == 1 ? key : mode == 11 ? null : value;
                require(failure == null && result == expected);
                if (operation == 0 && mode == 15) require(((Map) result).keySet().iterator().next() == first);
                if (operation == 0 && mode == 16) require(((Map) result).get(key) == second);
                if (operation == 0 && mode == 19) require(((Map) result).get(key) == key);
                if (operation == 0 && mode == 0) {
                    ((Map) result).put(sentinel, sentinel);
                    require(((Map) Trace.result).get(sentinel) == sentinel);
                }
                if (operation == 0 && mode == 14) {
                    try { ((Map) result).put(sentinel, sentinel); throw new AssertionError("immutable result changed"); }
                    catch (UnsupportedOperationException expectedFailure) { }
                }
            }
            System.out.println("route=" + route + ",operation=" + operation + ",mode=" + mode
                + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
