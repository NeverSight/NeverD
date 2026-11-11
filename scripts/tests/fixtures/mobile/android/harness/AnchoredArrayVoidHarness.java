import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.*;
public final class AnchoredArrayVoidHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("owned anchored array void"); }
    static void call(int route, boolean instance, Method method, Bucket sink, Object[] values) throws Throwable {
        if (route == 0) Owner.call(sink, values);
        else if (route == 1) AnchoredArrayVoidCaller.call(sink, values);
        else if (route == 2) {
            try { method.invoke(instance ? new Owner() : null, sink, values); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        } else {
            MethodHandle target = MethodHandles.privateLookupIn(Owner.class, MethodHandles.lookup()).unreflect(method);
            if (instance) target.invokeExact(new Owner(), sink, values);
            else target.invokeExact(sink, values);
        }
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        boolean instance = Boolean.parseBoolean(args[2]);
        Method method = Owner.class.getDeclaredMethod("append", Bucket.class, Object[].class);
        method.setAccessible(true);
        require(Modifier.isPrivate(method.getModifiers()) && Modifier.isStatic(method.getModifiers()) != instance);
        TypeVariable<?>[] formals = method.getTypeParameters();
        require(formals.length == 1 && formals[0].getBounds()[0] == Object.class && method.getGenericReturnType() == void.class);
        ParameterizedType carrier = (ParameterizedType)method.getGenericParameterTypes()[0];
        require(carrier.getRawType() == Bucket.class && carrier.getActualTypeArguments().length == 1
            && carrier.getActualTypeArguments()[0].equals(formals[0]));
        Type arrayType = method.getGenericParameterTypes()[1];
        require(arrayType instanceof GenericArrayType && ((GenericArrayType)arrayType).getGenericComponentType().equals(formals[0]));
        Object first = new Object(), last = new Object(), replacement = new Object();
        Object[] values = mode == 1 || mode == 12 || mode == 18 ? null
            : mode == 3 || mode == 21 ? new Object[0]
            : mode == 10 ? new String[]{"first",null,"last"}
            : mode == 11 ? new Object[][]{{first},null,{last}}
            : mode == 20 ? new Long[]{Long.MIN_VALUE,null,Long.MAX_VALUE}
            : mode == 17 ? new Object[]{first,first,null}
            : new Object[]{first,null,last};
        RuntimeException original = new IllegalStateException("owned partial append failure");
        Trace.failure = original;
        for (int attempt = 0; attempt < 2; attempt++) {
            final Object[] current = values;
            Object[] expected = values == null ? null : values.clone();
            Object[] readValues = expected == null ? null : expected.clone();
            List<Object> observed = new ArrayList<>(), kept = new ArrayList<>();
            Trace.events = 0; Trace.before = Trace.last = null;
            Trace.fail = mode == 6 || mode == 12 ? 1 : mode == 7 ? 2 : mode == 8 ? 3 : mode == 9 ? 4 : mode == 19 ? 9 : 0;
            Bucket<Object> callback = value -> {
                observed.add(value); Trace.hit(5);
                if ((mode == 14 || mode == 16) && observed.size() == 1) current[1] = replacement;
                if (mode == 15 && observed.size() == 1) current[0] = replacement;
                if (mode == 4 || mode == 5 && observed.size() == 2 || mode == 16) throw original;
                kept.add(value); return mode != 13;
            };
            Bucket sink = mode == 2 || mode == 3 || mode == 18 ? null : callback;
            Throwable failure = null;
            try { call(route, instance, method, sink, values); } catch (Throwable error) { failure = error; }
            if (mode == 19) {
                require(Trace.events == (attempt == 0 ? 9 : 0) && observed.isEmpty() && kept.isEmpty());
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == original);
                else require(failure instanceof NoClassDefFoundError);
            } else {
                int visits = mode == 1 || mode == 3 || mode == 6 || mode == 12 || mode == 18 || mode == 21 ? 0
                    : mode == 2 || mode == 4 || mode == 7 || mode == 8 || mode == 16 ? 1 : mode == 5 ? 2 : 3;
                int calls = mode == 2 || mode == 7 ? 0 : visits;
                int completed = mode == 4 || mode == 16 ? 0 : mode == 5 ? 1 : calls;
                require(observed.size() == calls && kept.size() == completed);
                if ((mode == 14 || mode == 16) && calls != 0) expected[1] = replacement;
                if (mode == 15 && calls != 0) expected[0] = replacement;
                for (int i = 0; i < calls; i++) {
                    Object value = mode == 14 && i == 1 ? replacement : readValues[i];
                    require(observed.get(i) == value && (i >= completed || kept.get(i) == value));
                }
                if (values != null) for (int i = 0; i < values.length; i++) require(values[i] == expected[i]);
                Object before = visits == 0 ? null : observed.size() == visits ? observed.get(visits - 1) : expected[visits - 1];
                require(Trace.before == before && Trace.last == (completed == 0 ? null : kept.get(completed - 1)));
                long events = attempt == 0 ? 9 : 0; events = events * 10 + 1;
                for (int i = 0; i < visits; i++) {
                    events = events * 10 + 2;
                    if (mode == 2 || mode == 7) break;
                    events = events * 10 + 5;
                    if (i >= completed) break;
                    events = events * 10 + 3;
                }
                boolean reachedEnd = mode == 0 || mode == 3 || mode >= 9 && mode != 12 && mode != 16 && mode != 18;
                if (reachedEnd) events = events * 10 + 4;
                require(Trace.events == events);
                if (mode == 1 || mode == 2 || mode == 18) require(failure instanceof NullPointerException);
                else if (mode >= 4 && mode <= 9 || mode == 12 || mode == 16) require(failure == original);
                else require(failure == null);
            }
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
