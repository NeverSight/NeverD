package probe;
import fixture.*;
import java.io.IOException;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ObjectPrefixDefaultsHarness {
    static Field flag, bits, cache;
    static void check(boolean value) {
        if (!value) throw new AssertionError("owned object prefix defaults mismatch");
    }
    @SuppressWarnings("unchecked")
    static <E extends Throwable> void raise(Throwable failure) throws E { throw (E) failure; }
    static void mutate(View owner, long value, Object[] array) {
        try { flag.setBoolean(owner, true); bits.setLong(owner, value); cache.set(owner, array); }
        catch (ReflectiveOperationException failure) { throw new AssertionError(failure); }
    }
    static void defaults(View owner, Object callback) {
        check(owner != null && !owner.flag() && owner.bits() == 0 && owner.cache() == null && owner.payload() == callback);
    }
    static View create(int route, Constructor<?> constructor, Fn<? super Input, Unit> callback) throws Throwable {
        if (route == 0) return Core.make(callback);
        if (route == 1) return ObjectPrefixDefaultsCaller.make(callback);
        Trace.hit(6);
        View result;
        try {
            if (route == 2) result = (View) constructor.newInstance(callback);
            else if (route == 3) {
                MethodHandle handle = MethodHandles.lookup().unreflectConstructor(constructor)
                    .asType(MethodType.methodType(View.class, Fn.class));
                result = (View) handle.invokeExact((Fn) callback);
            } else result = ObjectPrefixDefaultsCaller.direct(callback);
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
        Trace.hit(0);
        return result;
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        boolean finals = Boolean.parseBoolean(args[2]);
        Class<?> owner = Class.forName("fixture.Owner", false, ObjectPrefixDefaultsHarness.class.getClassLoader());
        check(Modifier.isFinal(owner.getModifiers()) && !Modifier.isPublic(owner.getModifiers()));
        check(owner.getSuperclass() == Object.class && java.util.Arrays.equals(owner.getInterfaces(), new Class<?>[]{View.class}));
        flag = owner.getDeclaredField("zeroFlag"); bits = owner.getDeclaredField("zeroBits"); cache = owner.getDeclaredField("zeroArray");
        for (Field field : new Field[]{flag, bits, cache}) {
            check(Modifier.isPrivate(field.getModifiers()) && Modifier.isFinal(field.getModifiers()) == finals && !field.isSynthetic());
            field.setAccessible(true);
        }
        Constructor<?> constructor = owner.getDeclaredConstructor(Fn.class);
        check(Modifier.isPublic(constructor.getModifiers()) && constructor.getExceptionTypes().length == 0);
        ParameterizedType parameter = (ParameterizedType) constructor.getGenericParameterTypes()[0];
        check(parameter.getRawType() == Fn.class && parameter.getActualTypeArguments()[1] == Unit.class);
        WildcardType input = (WildcardType) parameter.getActualTypeArguments()[0];
        check(java.util.Arrays.equals(input.getLowerBounds(), new java.lang.reflect.Type[]{Input.class}));
        check(java.util.Arrays.equals(input.getUpperBounds(), new java.lang.reflect.Type[]{Object.class}));
        constructor.setAccessible(true);
        for (int attempt = 0; attempt < 2; ++attempt) {
            Trace.guardAction = Trace.publishAction = null;
            long value = (attempt == 0 ? 0x7ff8123456abcdefL : 0x8123456789abcdefL) ^ mode;
            Unit unit = new Unit(new Object()), otherUnit = new Unit(new Object());
            int[] applied = {0};
            Fn<Object, Unit> callback = argument -> { applied[0]++; return unit; };
            Fn<Object, Unit> otherCallback = argument -> otherUnit;
            Object[] array = {unit, callback, unit};
            Object sentinel = new Object();
            Trace.argument = sentinel; Trace.label = null; Trace.partial = null; Trace.events = 0;
            int[] calls = {0};
            View[] published = {null}, nested = {null};
            Throwable failure = mode == 2 || mode == 5 ? new IOException("owned checked")
                : mode == 4 || mode == 7 ? new AssertionError("owned error") : new IllegalStateException("owned runtime");
            Runnable fail = () -> { calls[0]++; raise(failure); };
            if (mode >= 2 && mode <= 4) Trace.guardAction = fail;
            if (mode >= 5 && mode <= 7) Trace.publishAction = () -> {
                defaults(Trace.partial, callback);
                published[0] = Trace.partial;
                fail.run();
            };
            if (mode == 8) Trace.publishAction = () -> {
                calls[0]++;
                defaults(Trace.partial, callback);
                published[0] = Trace.partial;
                mutate(Trace.partial, value, array);
            };
            if (mode == 9) Trace.guardAction = () -> {
                calls[0]++;
                check(Trace.partial == null);
                Trace.guardAction = null;
                nested[0] = Core.make(otherCallback);
                defaults(nested[0], otherCallback);
            };
            if (mode == 10) Trace.publishAction = () -> {
                calls[0]++;
                defaults(Trace.partial, callback);
                published[0] = Trace.partial;
                Trace.publishAction = null;
                nested[0] = Core.make(otherCallback);
                defaults(nested[0], otherCallback);
                mutate(published[0], value, array);
            };
            View result = null;
            Throwable caught = null;
            try { result = create(route, constructor, mode == 1 ? null : callback); }
            catch (Throwable error) { caught = error; }
            boolean guarded = mode >= 1 && mode <= 4, aborted = mode >= 5 && mode <= 7;
            String events = "61";
            if (mode == 9) events += "61230";
            if (!guarded) events += "2";
            if (mode == 10) events += "61230";
            if (!guarded && !aborted) events += "30";
            check(Trace.events == Long.parseLong(events));
            check(Trace.argument == (mode == 9 || mode == 10 ? otherCallback : mode == 1 ? null : callback));
            check("owned".equals(Trace.label));
            check(calls[0] == (mode == 0 || mode == 1 || mode == 11 ? 0 : 1));
            if (guarded || aborted) {
                check(result == null);
                if (mode == 1) check(caught instanceof NullPointerException);
                else check(caught == failure);
                if (guarded) check(Trace.partial == null);
                else { check(Trace.partial == published[0]); defaults(published[0], callback); }
            } else {
                check(caught == null && result != null);
                check(Trace.partial == (mode == 10 ? nested[0] : result));
                if (mode == 8 || mode == 10) {
                    check(result == published[0] && result.flag() && result.bits() == value && result.cache() == array && result.payload() == callback);
                } else defaults(result, callback);
                if (mode == 9 || mode == 10) check(nested[0] != result && nested[0] != null);
                if (mode == 11) mutate(result, value, array);
                if (mode == 11) check(result.flag() && result.bits() == value && result.cache() == array);
                check(((Fn<Object, Unit>) result.payload()).apply(new Input(value)) == unit && applied[0] == 1);
            }
        }
        System.out.println("attempts=2,object-prefix-defaults=matched");
    }
}
