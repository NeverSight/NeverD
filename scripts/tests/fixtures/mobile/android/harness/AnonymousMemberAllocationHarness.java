import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class AnonymousMemberAllocationHarness {
    static void check(boolean condition) {
        if (!condition) throw new AssertionError("anonymous member allocation contract");
    }
    static GenericBase property(int route) throws Throwable {
        if (route == 0) return Client.property();
        if (route == 1) return AnonymousMemberAllocationCaller.property();
        if (route == 2) return (GenericBase) Owner.class.getField("PROPERTY").get(null);
        return (GenericBase) MethodHandles.lookup().findStaticGetter(Owner.class, "PROPERTY", GenericBase.class).invokeExact();
    }
    static Object get(int route, GenericBase property, Object owner) throws Throwable {
        try {
            if (route == 0) return property.get(owner);
            if (route == 1 && (owner == null || owner instanceof Owner))
                return AnonymousMemberAllocationCaller.get(property, (Owner) owner);
            if (route == 2 || route == 1) {
                Method bridge = property.getClass().getDeclaredMethod("get", Object.class);
                check(bridge.isBridge() && bridge.isSynthetic());
                bridge.setAccessible(true);
                return bridge.invoke(property, owner);
            }
            return (Object) MethodHandles.lookup().findVirtual(GenericBase.class, "get",
                    MethodType.methodType(Object.class, Object.class)).invokeExact(property, owner);
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    static void set(int route, GenericBase property, Object owner, Object value) throws Throwable {
        try {
            if (route == 0) property.set(owner, value);
            else if (route == 1 && (owner == null || owner instanceof Owner) && (value == null || value instanceof Float))
                AnonymousMemberAllocationCaller.set(property, (Owner) owner, (Float) value);
            else if (route == 2 || route == 1) {
                Method bridge = property.getClass().getDeclaredMethod("set", Object.class, Object.class);
                check(bridge.isBridge() && bridge.isSynthetic());
                bridge.setAccessible(true);
                bridge.invoke(property, owner, value);
            } else MethodHandles.lookup().findVirtual(GenericBase.class, "set",
                    MethodType.methodType(void.class, Object.class, Object.class)).invokeExact(property, owner, value);
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    static void failure(Throwable actual, Class<?> expected) {
        if (expected == null) check(actual == null);
        else {
            check(actual != null && actual.getClass() == expected);
            if (expected == Trace.failure.getClass()) check(actual == Trace.failure);
        }
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        boolean choose = args[2].equals("true"), conditional = args[3].equals("true"), recovered = args[4].equals("true");
        String first = new String("owned-first"), second = new String("owned-second");
        Trace.firstName = first; Trace.secondName = second; Trace.replacement = new String("owned-replaced");
        Trace.choose = choose; Trace.failAt = mode;
        Trace.failure = new IllegalStateException("owned anonymous allocation failure");
        GenericBase previous = null;
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0;
            GenericBase property = null; Throwable error = null;
            try { property = property(route); } catch (Throwable caught) { error = caught; }
            int initialized = mode == 1 ? 1 : mode == 2 ? 12 : mode == 3 ? 123 : 1234;
            check(Trace.events == (attempt == 0 ? initialized : 0));
            if (mode >= 1 && mode <= 4) {
                check(property == null);
                if (attempt == 0) check(error instanceof ExceptionInInitializerError && error.getCause() == Trace.failure);
                else check(error instanceof NoClassDefFoundError);
                check((Trace.constructed != null) == (mode >= 3));
            } else {
                check(error == null && property != null && property == Trace.constructed);
                if (attempt != 0) check(property == previous);
                previous = property;
                check(property.type == Float.class && property.name == (conditional && !choose ? second : first));
                Class<?> anonymous = property.getClass();
                check(anonymous.getDeclaredFields().length == 0 && anonymous.getEnclosingClass() == Owner.class
                        && anonymous.getEnclosingMethod() == null);
                check(anonymous.isAnonymousClass() != recovered && anonymous.isMemberClass() == recovered);
                check(!recovered || Modifier.isStatic(anonymous.getModifiers()));
                ParameterizedType parent = (ParameterizedType) anonymous.getGenericSuperclass();
                check(parent.getRawType() == GenericBase.class
                        && java.util.Arrays.equals(parent.getActualTypeArguments(), new java.lang.reflect.Type[]{Owner.class, Float.class}));
                Owner owner = new Owner();
                Float initial = Float.valueOf(-0.0f), next = Float.valueOf(Float.NaN);
                owner.value = initial;
                Object receiver = mode == 7 ? null : mode == 8 ? new Object() : owner;
                for (int operation = 0; operation < 3; operation++) {
                    Object result = null; Throwable problem = null;
                    try {
                        if (operation == 1) set(route, property, receiver, mode == 8 ? new Object() : next);
                        else result = get(route, property, receiver);
                    } catch (Throwable caught) { problem = caught; }
                    Class<?> expected = mode == 8 ? ClassCastException.class : mode == 7 ? NullPointerException.class
                            : (mode == 5 && operation != 1) || (mode == 6 && operation == 1) ? Trace.failure.getClass() : null;
                    failure(problem, expected);
                    if (expected == null && operation != 1)
                        check(result == (operation == 0 || mode == 6 ? initial : next));
                    check(owner.value == (operation == 0 || mode == 6 || mode == 7 || mode == 8 ? initial : next));
                }
                int trace = attempt == 0 ? 1234 : 0;
                if (mode != 8) trace = trace * 1000 + 565;
                check(Trace.events == trace);
            }
            System.out.println("route=" + route + ",mode=" + mode + ",choose=" + choose
                    + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
