import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class PartialFinalHarness {
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
    static Object packet(Object value) {
        Carrier<?> savedBox = Trace.box, savedAlternative = Trace.cellAlternative;
        Carrier<?> initialToken = new ValueCarrier(new Object()), afterCallback = new ValueCarrier(new Object());
        try {
            Trace.box = initialToken; Trace.cellAlternative = afterCallback;
            Class<?> cell = Class.forName("fixture.Scope$Cell");
            Constructor<?> primary = cell.getDeclaredConstructor(); primary.setAccessible(true);
            int before = Trace.events;
            Object empty = primary.newInstance();
            require(Trace.events == before * 10 + 7);
            require(Trace.seenCell == empty && Trace.cellValue == null && Trace.cellStamp == 0 && Trace.cellToken == null);
            require(Scope.value(empty) == null && Scope.stamp(empty) == Trace.events && Scope.token(empty) == null);
            Trace.box = initialToken; before = Trace.events;
            Object result = Scope.packet(value);
            require(Trace.events == before * 10 + 7);
            require(Trace.seenCell == result && Trace.cellValue == null && Trace.cellStamp == 0 && Trace.cellToken == null);
            require(Scope.value(result) == value && Scope.stamp(result) == Trace.events && Scope.token(result) == afterCallback);
            return result;
        } catch (ReflectiveOperationException failure) {
            throw new AssertionError("nullary carrier invocation changed", failure);
        } finally { Trace.box = savedBox; Trace.cellAlternative = savedAlternative; }
    }
    static void require(boolean value) { if (!value) throw new AssertionError("nullary carrier invocation changed"); }
    static Object call(int route, boolean checked) throws Throwable {
        if (route == 0) return checked ? Client.checked() : Client.call();
        if (route == 1) return checked ? PartialFinalCaller.checked() : PartialFinalCaller.call();
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
        Class<?> owner = Class.forName("fixture.Scope$Cell$Provider", true, Client.class.getClassLoader());
        Class<?> cell = Class.forName("fixture.Scope$Cell", false, Client.class.getClassLoader());
        require(Modifier.isPrivate(cell.getModifiers()));
        String encoded = System.getProperty("fixture.independent.encoded");
        if (encoded != null) {
            boolean input = Boolean.parseBoolean(encoded);
            require(Modifier.isStatic(cell.getModifiers()) != input && cell.isSynthetic() == input);
        }
        require(owner.getEnclosingClass() == cell && Modifier.isPublic(owner.getModifiers()) && Modifier.isStatic(owner.getModifiers()));
        String compilerOriginal = System.getProperty("fixture.partialfinal.original");
        if (compilerOriginal != null) {
            boolean originalFlags = Boolean.parseBoolean(compilerOriginal);
            require(Modifier.isFinal(cell.getDeclaredField("value").getModifiers()) != originalFlags);
            require(Modifier.isFinal(cell.getDeclaredField("token").getModifiers()) != originalFlags);
            require(cell.getDeclaredField("token").isSynthetic() != originalFlags);
        }
        require(Modifier.isFinal(cell.getDeclaredField("stamp").getModifiers()));
        Method method = owner.getDeclaredMethod("make", Carrier.class);
        TypeVariable<?>[] formals = method.getTypeParameters();
        require(!Modifier.isStatic(method.getModifiers()) && formals.length == 1 && formals[0].getBounds()[0] == Object.class);
        require(method.getReturnType() == Carrier.class && method.getParameterCount() == 1);
        require(((ParameterizedType)method.getGenericParameterTypes()[0]).getActualTypeArguments()[0].equals(formals[0]));
        Type inner = ((ParameterizedType)method.getGenericReturnType()).getActualTypeArguments()[0];
        require(inner instanceof ParameterizedType && ((ParameterizedType)inner).getRawType() == cell
            && ((ParameterizedType)inner).getActualTypeArguments()[0].equals(formals[0]));
        Trace.failAt = mode == 2 || mode == 3 ? mode : 0;
        Trace.failure = new RuntimeException("owned enclosing factory failure");
        int[] input = {17}; Trace.input = mode == 4 ? null : mode == 5 ? new Object() : input;
        String value = new String("owned result");
        Object payload = mode == 8 ? null : packet(mode == 9 ? new Object() : mode == 11 ? input : value);
        Object primary = new ValueCarrier(payload); alternateResult = new ValueCarrier(packet(value));
        Object sentinel = new Object();
        for (int attempt = 0; attempt < 2; attempt++) {
            Object receiver = mode == 1 ? null : mode == 15 ? new Object() : Scope.create();
            Object unseenReceiver = new Object();
            Trace.receiver = receiver; Trace.seenReceiver = unseenReceiver; Trace.alternativeReceiver = Scope.create();
            Trace.events = 0; Trace.after = sentinel; inputCalls = 0; resultCalls = 0;
            Carrier<?> original = mode == 12 ? null : new ProbeCarrier();
            Carrier<?> unseen = new ValueCarrier(new Object());
            Trace.box = original; Trace.seen = unseen; Trace.alternative = new ValueCarrier(new Object());
            Trace.result = mode == 6 ? new Object() : mode == 7 ? null : primary;
            int before = input[0]; Throwable failure = null; Object result = null;
            try { result = call(route, checked); } catch (Throwable problem) { failure = problem; }
            int trace = 0; boolean body = mode != 1 && mode != 15;
            if (body) trace = 2;
            boolean called = body && mode != 2 && mode != 12;
            if (called) trace = trace * 10 + 5;
            boolean mutated = called && mode != 13 && mode != 4 && mode != 5;
            if (mutated) trace = trace * 10 + 3;
            boolean returned = mutated && mode != 3 && mode != 6;
            if (returned) trace = trace * 10 + 4;
            require(Trace.seen == (body && mode != 2 ? original : unseen));
            require(Trace.seenReceiver == (body && mode != 2 ? receiver : unseenReceiver));
            require(inputCalls == (called ? 1 : 0));
            require(resultCalls == (returned && checked && mode != 7 ? 1 : 0));
            require(Trace.events == trace && input[0] == before + (mutated ? 1 : 0));
            require(Trace.after == (returned ? Trace.result : sentinel));
            if (mode == 2 || mode == 3 || mode == 13) require(failure == Trace.failure);
            else if (mode == 1 || mode == 4 || mode == 12 || (checked && (mode == 7 || mode == 8))) require(failure instanceof NullPointerException);
            else if (mode == 5 || mode == 6 || mode == 15 || (checked && (mode == 9 || mode == 11))) require(failure instanceof ClassCastException);
            else {
                require(failure == null && result == (checked ? value : Trace.result));
                if (!checked && mode == 11) require(Scope.value(((ValueCarrier)result).payload) == input);
                if (!checked && mode == 0) require(((ValueCarrier)result).payload == payload && Scope.value(payload) == value);
            }
            System.out.println("route=" + route + ",operation=" + operation + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
