import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.*;

public final class NestedPairConsumerHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("nested pair consumer factory changed");
    }
    static final class Input implements Iterable<Object> {
        final Object value;
        final boolean empty;
        Input(Object value, boolean empty) { this.value = value; this.empty = empty; }
        public Iterator<Object> iterator() {
            Trace.hit(7);
            return new Iterator<Object>() {
                boolean available = !empty;
                public boolean hasNext() { return available; }
                public Object next() {
                    Trace.hit(8);
                    if (!available) throw new NoSuchElementException();
                    available = false;
                    return value;
                }
            };
        }
    }
    static Object call(int route, int operation, Iterable<?> values) throws Throwable {
        if (route == 0) return operation == 0 ? Client.call(values)
            : operation == 1 ? Client.checkedKey(values) : Client.checkedValue(values);
        if (route == 1) return operation == 0 ? NestedPairConsumerCaller.call(values)
            : operation == 1 ? NestedPairConsumerCaller.checkedKey(values) : NestedPairConsumerCaller.checkedValue(values);
        String name = operation == 0 ? "call" : operation == 1 ? "checkedKey" : "checkedValue";
        if (route == 2) {
            try { return Client.class.getMethod(name, Iterable.class).invoke(null, values); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (operation != 0) return (String) MethodHandles.publicLookup().findStatic(Client.class, name,
            MethodType.methodType(String.class, Iterable.class)).invokeExact(values);
        return (Object) MethodHandles.publicLookup().findStatic(Client.class, name,
            MethodType.methodType(Object.class, Iterable.class)).invokeExact(values);
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        Class<?> owner = Class.forName("fixture.api.Provider", false, Client.class.getClassLoader());
        Method method = owner.getDeclaredMethod("make", Iterable.class);
        TypeVariable<?>[] formals = method.getTypeParameters();
        require(Modifier.isStatic(method.getModifiers()) && formals.length == 2);
        for (TypeVariable<?> formal : formals) require(formal.getBounds().length == 1
            && formal.getBounds()[0] == Object.class && formal.getGenericDeclaration().equals(method));
        ParameterizedType outer = (ParameterizedType) method.getGenericParameterTypes()[0];
        require(outer.getRawType() == Iterable.class && outer.getActualTypeArguments().length == 1);
        WildcardType bound = (WildcardType) outer.getActualTypeArguments()[0];
        require(bound.getLowerBounds().length == 0 && bound.getUpperBounds().length == 1);
        ParameterizedType inner = (ParameterizedType) bound.getUpperBounds()[0];
        require(inner.getRawType() == Map.class && inner.getActualTypeArguments().length == 2);
        for (int i = 0; i < 2; i++) {
            WildcardType slot = (WildcardType) inner.getActualTypeArguments()[i];
            require(slot.getLowerBounds().length == 0 && slot.getUpperBounds().length == 1
                && slot.getUpperBounds()[0].equals(formals[i]));
        }
        ParameterizedType resultType = (ParameterizedType) method.getGenericReturnType();
        require(resultType.getRawType() == Map.class && Arrays.equals(resultType.getActualTypeArguments(), formals));
        Trace.failAt = mode >= 1 && mode <= 6 ? mode : mode == 29 ? 7 : mode == 30 ? 8 : 0;
        Trace.failure = new RuntimeException("owned nested pair consumer failure");
        int[] first = mode == 14 ? new int[0] : new int[]{17};
        int[] second = mode == 17 ? new int[0] : mode == 25 ? first : new int[]{31};
        Object inputKey = mode == 12 ? null : mode == 13 ? new Object() : first;
        Object inputValue = mode == 15 ? null : mode == 16 ? new Object() : second;
        Map pair = mode == 10 ? null : new HashMap();
        if (pair != null && mode != 11) pair.put(inputKey, inputValue);
        Iterable<?> values = mode == 7 ? null : new Input(mode == 9 ? new Object() : pair, mode == 8);
        Trace.lookup = inputKey;
        Object key = mode == 21 ? new Object() : mode == 26 || mode == 24 ? first
            : mode == 31 ? null : new String("owned key");
        Object value = mode == 22 ? new Object() : mode == 27 || mode == 24 ? second
            : mode == 28 ? key : mode == 31 ? null : new String("owned value");
        Map resultMap = mode == 23 ? Collections.singletonMap(key, value) : mode == 24 ? pair : new HashMap();
        if (mode != 20 && mode != 23 && mode != 24) resultMap.put(key, value);
        Trace.key = key;
        Trace.result = mode == 18 ? new Object() : mode == 19 ? null : resultMap;
        Object sentinel = new Object();
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.after = sentinel; Trace.first = sentinel; Trace.second = sentinel;
            int beforeFirst = first.length == 0 ? 0 : first[0];
            int beforeSecond = second.length == 0 ? 0 : second[0];
            Throwable failure = null; Object result = null;
            try { result = call(route, operation, values); }
            catch (Throwable problem) { failure = problem; }
            int trace = attempt == 0 ? 1 : 0;
            boolean body = mode != 1;
            if (body) trace = trace * 10 + 2;
            boolean iterator = body && mode != 2 && mode != 7;
            if (iterator) trace = trace * 10 + 7;
            boolean next = iterator && mode != 29;
            if (next) trace = trace * 10 + 8;
            boolean gotPair = next && mode != 30 && mode != 8 && mode != 9;
            if (gotPair) trace = trace * 10 + 3;
            boolean firstMutation = gotPair && mode != 3 && mode != 10 && mode != 11
                && mode != 12 && mode != 13 && mode != 14;
            if (firstMutation) trace = trace * 10 + 4;
            boolean secondMutation = firstMutation && mode != 4 && mode != 15 && mode != 16 && mode != 17;
            if (secondMutation) trace = trace * 10 + 5;
            boolean returned = secondMutation && mode != 5 && mode != 18;
            if (returned) trace = trace * 10 + 6;
            require(Trace.events == trace && Trace.first == (body ? values : sentinel)
                && Trace.second == (gotPair ? pair : sentinel));
            if (first.length != 0) require(first[0] == beforeFirst + (firstMutation ? 1 : 0)
                + (first == second && secondMutation ? 3 : 0));
            if (second.length != 0) require(second[0] == beforeSecond + (secondMutation ? 3 : 0)
                + (first == second && firstMutation ? 1 : 0));
            require(Trace.after == (returned ? Trace.result : sentinel));
            if (mode == 1) {
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if ((mode >= 2 && mode <= 6) || mode == 29 || mode == 30) require(failure == Trace.failure);
            else if (mode == 7 || mode == 10 || mode == 12 || mode == 15 || (operation != 0 && mode == 19))
                require(failure instanceof NullPointerException);
            else if (mode == 8 || mode == 11 || (operation == 1 && mode == 20)) require(failure instanceof NoSuchElementException);
            else if (mode == 14 || mode == 17) require(failure instanceof ArrayIndexOutOfBoundsException);
            else if (mode == 9 || mode == 13 || mode == 16 || mode == 18
                    || (operation == 1 && (mode == 21 || mode == 24 || mode == 26))
                    || (operation == 2 && (mode == 22 || mode == 24 || mode == 27))) require(failure instanceof ClassCastException);
            else {
                Object expected = operation == 0 ? Trace.result : operation == 1 ? key : mode == 20 ? null : value;
                require(failure == null && result == expected);
                if (operation == 0 && mode == 24) require(result == pair && ((Map) result).get(first) == second);
                if (operation == 0 && mode == 26) require(((Map) result).keySet().iterator().next() == first);
                if (operation == 0 && mode == 27) require(((Map) result).get(key) == second);
                if (operation == 0 && mode == 28) require(((Map) result).get(key) == key);
                if (operation == 0 && mode == 0) {
                    ((Map) result).put(sentinel, sentinel);
                    require(((Map) Trace.result).get(sentinel) == sentinel);
                }
                if (operation == 0 && mode == 23) {
                    try { ((Map) result).put(sentinel, sentinel); throw new AssertionError("immutable result changed"); }
                    catch (UnsupportedOperationException expectedFailure) { }
                }
            }
            System.out.println("route=" + route + ",operation=" + operation + ",mode=" + mode
                + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
