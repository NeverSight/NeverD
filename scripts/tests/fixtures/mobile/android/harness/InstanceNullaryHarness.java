import fixture.Client;
import fixture.Pair;
import fixture.Provider;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.lang.reflect.TypeVariable;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class InstanceNullaryHarness {
    static void check(boolean condition) {
        if (!condition) throw new AssertionError("owned instance nullary factory changed");
    }
    static void metadata() throws Exception {
        Method method = Provider.class.getDeclaredMethod("make");
        check(Modifier.isPublic(method.getModifiers()) && !Modifier.isStatic(method.getModifiers()) && !method.isSynthetic() && !method.isVarArgs());
        check(Provider.class.getTypeParameters().length == 0 && method.getParameterCount() == 0 && method.getExceptionTypes().length == 0);
        TypeVariable<?>[] formals = method.getTypeParameters(); check(formals.length == 2 && !formals[0].equals(formals[1]));
        for (TypeVariable<?> formal : formals) check(formal.getGenericDeclaration().equals(method) && formal.getBounds().length == 1 && formal.getBounds()[0] == Object.class);
        check(method.getReturnType() == Pair.class && method.getGenericReturnType() instanceof ParameterizedType);
        ParameterizedType result = (ParameterizedType)method.getGenericReturnType();
        Type[] slots = result.getActualTypeArguments();
        check(result.getRawType() == Pair.class && slots.length == 2 && slots[0].equals(formals[0]) && slots[1].equals(formals[1]));
    }
    static Object invoke(int route, int operation) throws Throwable {
        if (route == 0) return operation == 0 ? Client.call() : operation == 1 ? Client.checkedKey() : Client.checkedValue();
        if (route == 1) return operation == 0 ? InstanceNullaryCaller.call() : operation == 1 ? InstanceNullaryCaller.checkedKey() : InstanceNullaryCaller.checkedValue();
        if (route == 2) {
            String name = operation == 0 ? "call" : operation == 1 ? "checkedKey" : "checkedValue";
            try { return Client.class.getMethod(name).invoke(null); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        Pair result;
        if (route == 3) {
            try { result = (Pair)Provider.class.getMethod("make").invoke(Trace.current); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        } else result = (Pair)MethodHandles.lookup().findVirtual(Provider.class, "make", MethodType.methodType(Pair.class)).invokeExact(Trace.current);
        Trace.after = result; Trace.hit(4);
        return operation == 0 ? result : operation == 1 ? (String)result.key : (String)result.value;
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]); metadata();
        boolean derived = mode == 1 || mode == 6;
        Provider original = mode == 14 ? null : derived ? new InstanceNullaryDerived() : new Provider();
        Provider other = mode == 14 ? null : new Provider();
        Object otherValue = new Object(); if (other != null) other.value = otherValue;
        for (int attempt = 0; attempt < 2; attempt++) {
            Object key = mode == 9 ? new Object() : new String("key");
            Object value = mode == 10 ? new Object() : mode == 13 ? key : new String("value");
            Pair pair = new Pair(key, value); Pair late = new Pair(new Object(), new Object());
            Object supplied = mode == 7 ? new Object() : mode == 8 ? null : pair;
            if (original != null) original.value = supplied;
            Trace.current = mode == 2 || mode == 14 ? null : original; Trace.other = other; Trace.seen = null;
            Object sentinel = new Object(); Trace.after = sentinel; Trace.late = late;
            Trace.failure = new IllegalArgumentException("original"); Trace.error = new LinkageError("original");
            Trace.fail = mode == 3 ? 2 : mode == 4 ? 3 : mode == 5 ? 4 : mode == 6 ? 5 : mode == 11 ? -2 : mode == 12 ? -3 : 0;
            Trace.events = 0; int before = original == null ? 0 : original.calls;
            Throwable failure = null; Object result = null;
            try { result = invoke(route, operation); } catch (Throwable caught) { failure = caught; }
            boolean entered = mode != 2 && mode != 14 && mode != 6;
            boolean counted = entered && mode != 3 && mode != 11;
            boolean cast = counted && mode != 7;
            boolean returned = cast && mode != 4 && mode != 12;
            long events = derived ? 5 : 0;
            if (entered) events = events * 11 + 2;
            if (cast) events = events * 11 + 3;
            if (returned) events = events * 11 + 4;
            check(Trace.events == events && Trace.current == (entered ? other : mode == 2 || mode == 14 ? null : original));
            check(original == null || original.calls == before + (counted ? 1 : 0));
            check(Trace.seen == (counted ? original : null));
            check(original == null || original.value == (cast ? late : supplied));
            check(other == null || other.calls == 0 && other.value == otherValue);
            check(Trace.after == (returned ? supplied : sentinel));
            if (mode == 3 || mode == 4 || mode == 5 || mode == 6) check(failure == Trace.failure);
            else if (mode == 11 || mode == 12) check(failure == Trace.error);
            else if (mode == 2 || mode == 14 || mode == 8 && operation != 0) check(failure instanceof NullPointerException);
            else if (mode == 7 || mode == 9 && operation == 1 || mode == 10 && operation == 2) check(failure instanceof ClassCastException);
            else check(failure == null && result == (operation == 0 ? supplied : operation == 1 ? key : value));
            check(pair.key == key && pair.value == value);
            System.out.println("route=" + route + ",operation=" + operation + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
