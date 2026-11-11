import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.*;
public final class NestedInvariantHarness {
    static int mode, inputCalls, resultCalls;
    static Object alternateResult;
    static final class ValueCarrier implements Carrier<Object> {
        final Object payload;
        ValueCarrier(Object payload) { this.payload = payload; }
        public Object touch() { resultCalls++; return payload; }
    }
    static final class ProbeCarrier implements Carrier<Object> {
        public Object touch() {
            inputCalls++; Trace.hit(5);
            if (mode == 13) throw Trace.failure;
            if (mode == 14) Trace.result = alternateResult;
            return new Object();
        }
    }
    static void require(boolean value) { if (!value) throw new AssertionError("nullary carrier invocation changed"); }
    static Object call(int route, boolean checked) throws Throwable {
        if (route == 0) return checked ? Client.checked() : Client.call();
        if (route == 1) return checked ? NestedInvariantCaller.checked() : NestedInvariantCaller.call();
        String name = checked ? "checked" : "call";
        if (route == 2) {
            try { return Client.class.getMethod(name).invoke(null); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (checked) return (String) MethodHandles.publicLookup().findStatic(Client.class, name,
                MethodType.methodType(String.class)).invokeExact();
        return (Object) MethodHandles.publicLookup().findStatic(Client.class, name,
                MethodType.methodType(Object.class)).invokeExact();
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]); mode = Integer.parseInt(args[2]);
        boolean checked = operation == 1;
        Class<?> owner = Class.forName("fixture.api.Provider", false, Client.class.getClassLoader());
        Method method = owner.getDeclaredMethod("make", Carrier.class);
        TypeVariable<?>[] formals = method.getTypeParameters();
        require(Modifier.isStatic(method.getModifiers()) && formals.length == 1 && formals[0].getBounds()[0] == Object.class);
        require(method.getParameterCount() == 1 && method.getReturnType() == Carrier.class);
        require(((ParameterizedType)method.getGenericParameterTypes()[0]).getActualTypeArguments()[0].equals(formals[0]));
        Type inner = ((ParameterizedType)method.getGenericReturnType()).getActualTypeArguments()[0];
        require(inner instanceof ParameterizedType && ((ParameterizedType)inner).getRawType() == List.class
            && ((ParameterizedType)inner).getActualTypeArguments()[0].equals(formals[0]));
        Trace.failAt = mode >= 1 && mode <= 3 ? mode : 0;
        Trace.failure = new RuntimeException("owned nullary factory failure");
        int[] input = {17};
        Trace.input = mode == 4 ? null : mode == 5 ? new Object() : input;
        String value = new String("owned result");
        ArrayList list = new ArrayList();
        if (mode != 8) list.add(mode == 9 ? new Object() : mode == 11 ? input : value);
        Object payload = mode == 10 ? Collections.singletonList(value) : list;
        Object primary = new ValueCarrier(payload);
        alternateResult = new ValueCarrier(Collections.singletonList(value));
        Object sentinel = new Object();
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.after = sentinel; inputCalls = 0; resultCalls = 0;
            Carrier<?> original = mode == 12 ? null : new ProbeCarrier();
            Carrier<?> unseen = new ValueCarrier(new Object());
            Trace.box = original; Trace.seen = unseen; Trace.alternative = new ValueCarrier(new Object());
            Trace.result = mode == 6 ? new Object() : mode == 7 ? null : primary;
            int before = input[0];
            Throwable failure = null; Object result = null;
            try { result = call(route, checked); } catch (Throwable problem) { failure = problem; }
            int trace = attempt == 0 ? 1 : 0;
            boolean body = mode != 1;
            if (body) trace = trace * 10 + 2;
            boolean called = body && mode != 2 && mode != 12;
            if (called) trace = trace * 10 + 5;
            boolean mutated = called && mode != 13 && mode != 4 && mode != 5;
            if (mutated) trace = trace * 10 + 3;
            boolean returned = mutated && mode != 3 && mode != 6;
            if (returned) trace = trace * 10 + 4;
            require(Trace.seen == (body && mode != 2 ? original : unseen));
            require(inputCalls == (called ? 1 : 0));
            require(resultCalls == (returned && checked && mode != 7 ? 1 : 0));
            require(Trace.events == trace && input[0] == before + (mutated ? 1 : 0));
            require(Trace.after == (returned ? Trace.result : sentinel));
            if (mode == 1) {
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (mode == 2 || mode == 3 || mode == 13) require(failure == Trace.failure);
            else if (mode == 4 || mode == 12 || (checked && mode == 7)) require(failure instanceof NullPointerException);
            else if (mode == 5 || mode == 6 || (checked && (mode == 9 || mode == 11))) require(failure instanceof ClassCastException);
            else if (checked && mode == 8) require(failure instanceof IndexOutOfBoundsException);
            else {
                require(failure == null && result == (checked ? value : Trace.result));
                if (!checked && mode == 11) require(((List)((ValueCarrier)result).payload).get(0) == input);
                if (!checked && mode == 0) {
                    ((List)((ValueCarrier)result).payload).add(sentinel);
                    require(((List)((ValueCarrier)Trace.result).payload).get(((List)((ValueCarrier)Trace.result).payload).size() - 1) == sentinel);
                }
                if (!checked && mode == 10) {
                    try { ((List)((ValueCarrier)result).payload).add(sentinel); throw new AssertionError("immutable result changed"); }
                    catch (UnsupportedOperationException expected) { }
                }
            }
            System.out.println("route=" + route + ",operation=" + operation + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
