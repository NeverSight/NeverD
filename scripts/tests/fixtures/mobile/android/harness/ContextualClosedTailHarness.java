import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.*;

public final class ContextualClosedTailHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("nullary carrier invocation changed");
    }
    static Object call(int route, int operation) throws Throwable {
        if (route == 0) return operation == 0 ? Client.call()
            : operation == 1 ? Client.checkedKey() : Client.checkedValue();
        if (route == 1) return operation == 0 ? ContextualClosedTailCaller.call()
            : operation == 1 ? ContextualClosedTailCaller.checkedKey() : ContextualClosedTailCaller.checkedValue();
        String name = operation == 0 ? "call" : operation == 1 ? "checkedKey" : "checkedValue";
        if (route == 2) {
            try { return Client.class.getMethod(name).invoke(null); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (operation != 0) return (String) MethodHandles.publicLookup().findStatic(Client.class, name,
            MethodType.methodType(String.class)).invokeExact();
        return (Object) MethodHandles.publicLookup().findStatic(Client.class, name,
            MethodType.methodType(Object.class)).invokeExact();
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        Class<?> owner = Class.forName("fixture.api.Provider", false, Client.class.getClassLoader());
        Method method = owner.getDeclaredMethod("make",List.class,Config.class);
        TypeVariable<?>[] formals = method.getTypeParameters();
        require(Modifier.isStatic(method.getModifiers()) && formals.length == 1);
        for (TypeVariable<?> formal : formals) require(formal.getBounds().length == 1
            && formal.getBounds()[0] == Object.class && formal.getGenericDeclaration().equals(method));
        require(method.getParameterCount() == 2 && method.getReturnType() == Map.class);
        Type[] results = ((ParameterizedType) method.getGenericReturnType()).getActualTypeArguments();
        require(results.length == 2 && results[0].equals(formals[0]) && results[1] == String.class);
        ParameterizedType producer=(ParameterizedType)method.getGenericParameterTypes()[0];
        require(producer.getRawType()==List.class && producer.getActualTypeArguments()[0].equals(formals[0]) && method.getGenericParameterTypes()[1]==Config.class);
        Trace.failAt = mode >= 1 && mode <= 3 ? mode : 0;
        Trace.failure = new RuntimeException("owned nullary pair failure");
        int[] input = {17};
        Trace.input = mode == 4 ? null : mode == 5 ? new Object() : input;
        Object key = mode == 9 ? new Object() : mode == 12 ? input
            : mode == 15 ? null : new String("owned key");
        Object value = mode == 11 ? new Object() : mode == 13 ? input
            : mode == 14 ? key : mode == 15 ? null : new String("owned value");
        Map map = new HashMap();
        if (mode != 8) map.put(key, value);
        Trace.key = key;
        Trace.result = mode == 6 ? new Object() : mode == 7 ? null
            : mode == 10 ? Collections.singletonMap(key, value) : map;
        Trace.source=mode==16?null:new ArrayList(Arrays.asList(new Object()));
        Trace.context=mode==17?null:new Config(mode==8?Long.MIN_VALUE:0x1234567887654321L);
        Object sentinel = new Object();
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.after = sentinel;Trace.seenSource=Trace.seenContext=sentinel;Trace.snapshot=73;
            int before = input[0];
            Throwable failure = null; Object result = null;
            try { result = call(route, operation); } catch (Throwable problem) { failure = problem; }
            int trace = attempt == 0 ? 1 : 0;
            boolean body = mode != 1;
            if (body) trace = trace * 10 + 2;
            boolean mutated = body && mode != 2 && mode != 4 && mode != 5 && mode != 17;
            if (mutated) trace = trace * 10 + 3;
            boolean returned = mutated && mode != 3 && mode != 6;
            if (returned) trace = trace * 10 + 4;
            require(Trace.events == trace && input[0] == before + (mutated ? 1 : 0));
            require(Trace.after == (returned ? Trace.result : sentinel));
            boolean captured=body && mode!=2;
            require(Trace.seenSource==(captured?Trace.source:sentinel) && Trace.seenContext==(captured?Trace.context:sentinel));
            require(Trace.snapshot==(captured && mode!=17?Trace.context.mark:73));
            if (mode == 1) {
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (mode == 2 || mode == 3) require(failure == Trace.failure);
            else if (mode == 4 || mode == 17 || (operation != 0 && mode == 7)) require(failure instanceof NullPointerException);
            else if (mode == 5 || mode == 6 || (operation == 1 && (mode == 9 || mode == 12))
                    || (operation == 2 && (mode == 11 || mode == 13))) require(failure instanceof ClassCastException);
            else if (operation == 1 && mode == 8) require(failure instanceof NoSuchElementException);
            else {
                Object expected = operation == 0 ? Trace.result : operation == 1 ? key : mode == 8 ? null : value;
                require(failure == null && result == expected);
                if (operation == 0 && mode == 12) require(((Map) result).keySet().iterator().next() == input);
                if (operation == 0 && mode == 13) require(((Map) result).get(key) == input);
                if (operation == 0 && mode == 14) require(((Map) result).get(key) == key);
                if (operation == 0 && mode == 0) {
                    ((Map) result).put(sentinel, sentinel);
                    require(((Map) Trace.result).get(sentinel) == sentinel);
                }
                if (operation == 0 && mode == 10) {
                    try { ((Map) result).put(sentinel, sentinel); throw new AssertionError("immutable result changed"); }
                    catch (UnsupportedOperationException expectedFailure) { }
                }
            }
            System.out.println("route=" + route + ",operation=" + operation + ",mode=" + mode
                + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
