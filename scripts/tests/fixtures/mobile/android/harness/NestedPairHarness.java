import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.*;

public final class NestedPairHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("nested pair factory changed");
    }
    static final class Input implements Iterable<Object> {
        final Object value;
        final int event;
        final boolean empty;
        Input(Object value, int event, boolean empty) { this.value = value; this.event = event; this.empty = empty; }
        public Iterator<Object> iterator() {
            Trace.hit(event);
            return new Iterator<Object>() {
                boolean available = !empty;
                public boolean hasNext() { return available; }
                public Object next() {
                    Trace.hit(event + 1);
                    if (!available) throw new NoSuchElementException();
                    available = false;
                    return value;
                }
            };
        }
    }
    static Object call(int route, int operation, Iterable<?> first, Iterable<?> second) throws Throwable {
        if (route == 0) return operation == 0 ? Client.call(first, second)
            : operation == 1 ? Client.checkedKey(first, second) : Client.checkedValue(first, second);
        if (route == 1) return operation == 0 ? NestedPairCaller.call(first, second)
            : operation == 1 ? NestedPairCaller.checkedKey(first, second) : NestedPairCaller.checkedValue(first, second);
        String name = operation == 0 ? "call" : operation == 1 ? "checkedKey" : "checkedValue";
        if (route == 2) {
            try { return Client.class.getMethod(name, Iterable.class, Iterable.class).invoke(null, first, second); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (operation != 0) return (String) MethodHandles.publicLookup().findStatic(Client.class, name,
            MethodType.methodType(String.class, Iterable.class, Iterable.class)).invokeExact(first, second);
        return (Object) MethodHandles.publicLookup().findStatic(Client.class, name,
            MethodType.methodType(Object.class, Iterable.class, Iterable.class)).invokeExact(first, second);
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        Class<?> owner = Class.forName("fixture.api.Provider", false, Client.class.getClassLoader());
        Method method = owner.getDeclaredMethod("make", Iterable.class, Iterable.class);
        TypeVariable<?>[] formals = method.getTypeParameters();
        require(Modifier.isStatic(method.getModifiers()) && formals.length == 2);
        for (TypeVariable<?> formal : formals) require(formal.getBounds().length == 1
            && formal.getBounds()[0] == Object.class && formal.getGenericDeclaration().equals(method));
        Type[] inputs = method.getGenericParameterTypes();
        require(inputs.length == 2);
        for (int i = 0; i < 2; i++) {
            ParameterizedType carrier = (ParameterizedType) inputs[i];
            Type[] values = carrier.getActualTypeArguments();
            require(carrier.getRawType() == Iterable.class && values.length == 1);
            WildcardType value = (WildcardType) values[0];
            require(value.getLowerBounds().length == 0 && value.getUpperBounds().length == 1
                && value.getUpperBounds()[0].equals(formals[i]));
        }
        ParameterizedType outer = (ParameterizedType) method.getGenericReturnType();
        require(outer.getRawType() == List.class && outer.getActualTypeArguments().length == 1);
        ParameterizedType inner = (ParameterizedType) outer.getActualTypeArguments()[0];
        require(inner.getRawType() == Map.class && Arrays.equals(inner.getActualTypeArguments(), formals));
        Trace.failAt = mode >= 1 && mode <= 4 ? mode : mode == 22 ? 5 : mode >= 27 ? mode - 21 : 0;
        Trace.failure = new RuntimeException("owned nested pair failure");
        int[] first = mode == 20 ? new int[0] : new int[]{17};
        int[] second = mode == 21 ? new int[0] : mode == 17 ? first : new int[]{31};
        Iterable<?> firstInput = mode == 5 ? null : new Input(mode == 6 ? new Object() : first, 6, mode == 25);
        Iterable<?> secondInput = mode == 7 ? null : mode == 17 ? firstInput
            : new Input(mode == 8 ? new Object() : second, 8, mode == 26);
        Object key = mode == 12 ? new Object() : mode == 15 ? first
            : mode == 18 ? null : new String("owned key");
        Object value = mode == 13 ? new Object() : mode == 16 ? second
            : mode == 19 ? key : mode == 18 ? null : new String("owned value");
        Map map = mode == 14 ? Collections.singletonMap(key, value) : new HashMap();
        if (mode != 11 && mode != 14) map.put(key, value);
        Trace.key = key;
        List list = new ArrayList();
        if (mode != 23) list.add(mode == 24 ? new Object() : map);
        Trace.result = mode == 9 ? new Object() : mode == 10 ? null
            : mode == 14 ? Collections.singletonList(map) : list;
        Object sentinel = new Object();
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.after = sentinel; Trace.first = sentinel; Trace.second = sentinel;
            int beforeFirst = first.length == 0 ? 0 : first[0];
            int beforeSecond = second.length == 0 ? 0 : second[0];
            Throwable failure = null; Object result = null;
            try { result = call(route, operation, firstInput, secondInput); }
            catch (Throwable problem) { failure = problem; }
            int trace = attempt == 0 ? 1 : 0;
            boolean body = mode != 1;
            if (body) trace = trace * 10 + 2;
            boolean firstIterator = body && mode != 2 && mode != 5;
            if (firstIterator) trace = trace * 10 + 6;
            boolean firstNext = firstIterator && mode != 27;
            if (firstNext) trace = trace * 10 + 7;
            boolean firstMutation = firstNext && mode != 28 && mode != 6 && mode != 20 && mode != 25;
            if (firstMutation) trace = trace * 10 + 3;
            boolean secondIterator = firstMutation && mode != 3 && mode != 7;
            if (secondIterator) trace = trace * 10 + (mode == 17 ? 6 : 8);
            boolean secondNext = secondIterator && mode != 29;
            if (secondNext) trace = trace * 10 + (mode == 17 ? 7 : 9);
            boolean secondMutation = secondNext && mode != 30 && mode != 8 && mode != 21 && mode != 26;
            if (secondMutation) trace = trace * 10 + 4;
            boolean returned = secondMutation && mode != 4 && mode != 9;
            if (returned) trace = trace * 10 + 5;
            require(Trace.events == trace && Trace.first == (body ? firstInput : sentinel)
                && Trace.second == (body ? secondInput : sentinel));
            if (first.length != 0) require(first[0] == beforeFirst + (firstMutation ? 1 : 0)
                + (first == second && secondMutation ? 3 : 0));
            if (second.length != 0) require(second[0] == beforeSecond + (secondMutation ? 3 : 0)
                + (first == second && firstMutation ? 1 : 0));
            require(Trace.after == (returned ? Trace.result : sentinel));
            if (mode == 1) {
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (mode == 2 || mode == 3 || mode == 4 || mode == 22 || mode >= 27) require(failure == Trace.failure);
            else if (mode == 5 || mode == 7 || (operation != 0 && mode == 10)) require(failure instanceof NullPointerException);
            else if (mode == 6 || mode == 8 || mode == 9 || (operation != 0 && mode == 24)
                    || (operation == 1 && (mode == 12 || mode == 15))
                    || (operation == 2 && (mode == 13 || mode == 16))) require(failure instanceof ClassCastException);
            else if (mode == 20 || mode == 21) require(failure instanceof ArrayIndexOutOfBoundsException);
            else if (mode == 25 || mode == 26 || (operation == 1 && mode == 11)) require(failure instanceof NoSuchElementException);
            else if (operation != 0 && mode == 23) require(failure instanceof IndexOutOfBoundsException);
            else {
                Object expected = operation == 0 ? Trace.result : operation == 1 ? key : mode == 11 ? null : value;
                require(failure == null && result == expected);
                if (operation == 0 && mode != 10 && mode != 23 && mode != 24)
                    require(((List) result).get(0) == map);
                if (operation == 0 && mode == 15) require(((Map) ((List) result).get(0)).keySet().iterator().next() == first);
                if (operation == 0 && mode == 16) require(((Map) ((List) result).get(0)).get(key) == second);
                if (operation == 0 && mode == 19) require(((Map) ((List) result).get(0)).get(key) == key);
                if (operation == 0 && mode == 0) {
                    ((List) result).add(sentinel);
                    map.put(sentinel, sentinel);
                    require(((List) Trace.result).get(((List) Trace.result).size() - 1) == sentinel
                        && ((Map) ((List) Trace.result).get(0)).get(sentinel) == sentinel);
                }
                if (operation == 0 && mode == 14) {
                    try { ((List) result).add(sentinel); throw new AssertionError("immutable outer changed"); }
                    catch (UnsupportedOperationException expectedFailure) { }
                    try { map.put(sentinel, sentinel); throw new AssertionError("immutable inner changed"); }
                    catch (UnsupportedOperationException expectedFailure) { }
                }
            }
            System.out.println("route=" + route + ",operation=" + operation + ",mode=" + mode
                + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
