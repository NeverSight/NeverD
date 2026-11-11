import fixture.Api;
import fixture.Box;
import fixture.Core;
import fixture.Child;
import fixture.Marker;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;
import java.util.ArrayList;
import java.util.NoSuchElementException;
import java.util.function.Consumer;

public final class AbstractBoxedGenericResultsHarness {
    private static int values, identities, states, failures;
    private static final Method NEXT, ROOT, EACH, TYPED;
    private static final MethodHandle TYPED_HANDLE;
    private static final MethodHandle HANDLE;
    static {
        try {
            NEXT = java.util.Arrays.stream(Core.class.getDeclaredMethods()).filter(
                m -> m.getName().equals("next") && m.getReturnType() == Object.class).findFirst().get();
            String typedName = System.getProperty("owned.typed.name", "next");
            TYPED = Core.class.getDeclaredMethod(typedName);
            if (TYPED.getReturnType() != Box.class) throw new AssertionError("typed slot lost");
            TYPED_HANDLE = MethodHandles.lookup().findVirtual(Core.class, typedName,
                MethodType.methodType(Box.class));
            ROOT = Api.class.getMethod("next");
            EACH = Api.class.getMethod("forEachRemaining", Consumer.class);
            HANDLE = MethodHandles.lookup().findVirtual(Api.class, "next",
                MethodType.methodType(Object.class));
        } catch (Exception error) { throw new AssertionError(error); }
    }
    private static void state(int expected, int actual) {
        if (expected != actual) throw new AssertionError("state changed " + expected + ":" + actual);
        states++;
    }
    private static void same(Object expected, Object actual) {
        if (expected != actual) throw new AssertionError("identity changed");
        identities++;
    }
    private static Object call(int route, Core core) throws Throwable {
        try {
            switch (route) {
                case 0: return core.next();
                case 1: return ((Api) core).next();
                case 2: return ((Api<Box>) (Api) core).next();
                case 3: return NEXT.invoke(core);
                case 4: return ROOT.invoke(core);
                case 5: return (Object) HANDLE.invokeExact((Api) core);
                case 6: return TYPED.invoke(core);
                case 7: return (Box) TYPED_HANDLE.invokeExact(core);
                default: throw new AssertionError();
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void reset(Object marker) {
        Box.calls = 0;
        Box.lastInput = 77;
        Box.marker = marker;
        Box.observed = null;
        Box.failure = null;
        Box.cached = null;
        Box.last = null;
        Box.returnNull = false;
    }
    private static void metadata(boolean original) throws Exception {
        ParameterizedType edge = (ParameterizedType) Core.class.getGenericInterfaces()[0];
        if (Boolean.getBoolean("owned.deferred.slot")) {
            if (java.util.Arrays.stream(Core.class.getDeclaredMethods()).anyMatch(m -> m.getName().equals("hasNext"))
                    || Child.class.getDeclaredMethod("hasNext").getReturnType() != boolean.class
                    || java.lang.reflect.Modifier.isAbstract(Child.class.getDeclaredMethod("hasNext").getModifiers()))
                throw new AssertionError("deferred plain slot acquired a parent body or changed child ownership");
        }
        if (edge.getRawType() != Api.class || edge.getActualTypeArguments()[0] != (original ? Box.class : Object.class)
                || Core.class.getInterfaces()[1] != Marker.class
                || NEXT.getReturnType() != Object.class || NEXT.getGenericReturnType() != Object.class
                || NEXT.isBridge() != original || NEXT.isSynthetic() != original
                || !java.lang.reflect.Modifier.isAbstract(Core.class.getModifiers())
                || !java.lang.reflect.Modifier.isAbstract(Core.class.getMethod("readValue").getModifiers())
                || !java.lang.reflect.Modifier.isFinal(TYPED.getModifiers())
                || Child.class.getDeclaredMethod("readValue").getDeclaringClass() != Child.class
                || !ROOT.getGenericReturnType().equals(Api.class.getTypeParameters()[0])
                || !Core.class.getMethod("echo", Object.class).getGenericReturnType()
                    .equals(Core.class.getMethod("echo", Object.class).getTypeParameters()[0]))
            throw new AssertionError("complete interface, runtime entry or independent method scope changed");
    }
    private static void success(int route, long input, int mode) throws Throwable {
        int[] marker = {7};
        reset(marker);
        Core core = new Child(new long[]{input});
        core.marker = marker;
        Box cached = Box.box(input);
        reset(marker);
        marker[0] = 7;
        if (mode == 1) Box.cached = cached;
        if (mode == 2) Box.returnNull = true;
        Object result = call(route, core);
        if (mode == 2 ? result != null : !(result instanceof Box) || ((Box) result).value != input)
            throw new AssertionError("boxed result bits changed");
        values++;
        if (Box.lastInput != input) throw new AssertionError("wide factory argument changed");
        values++;
        if (mode == 0 && result == cached) throw new AssertionError("new factory allocation became cached");
        if (mode == 1) same(cached, result); else same(Box.last, result);
        same(marker, core.marker); same(marker, core.observed);
        same(marker, Box.marker); same(marker, Box.observed);
        state(1, core.calls); state(1, core.index); state(1, Box.calls); state(15, marker[0]);
    }
    private static void failed(int route, int kind) throws Throwable {
        int[] marker = {11}; reset(marker);
        Core core = new Child(new long[]{Long.MIN_VALUE}); core.marker = marker;
        RuntimeException target = new RuntimeException("owned-target");
        RuntimeException factory = new RuntimeException("owned-factory");
        if (kind == 0) core.failure = target;
        if (kind == 1) Box.failure = factory;
        if (kind == 2) core.index = 1;
        try { call(route, kind == 3 ? null : core); throw new AssertionError("failure missing"); }
        catch (Throwable error) {
            if (error instanceof AssertionError) throw error;
            if (kind == 0 || kind == 1) same(kind == 0 ? target : factory, error);
            else if (kind == 2 ? !(error instanceof NoSuchElementException) : !(error instanceof NullPointerException))
                throw new AssertionError("failure order changed", error);
            failures++;
        }
        state(kind == 3 ? 0 : 1, core.calls);
        state(kind == 1 || kind == 2 ? 1 : 0, core.index);
        state(kind == 1 ? 1 : 0, Box.calls);
        state(11 + (kind == 3 ? 0 : kind == 1 ? 8 : 3), marker[0]);
        same(marker, core.marker); same(kind == 3 ? null : marker, core.observed);
        same(marker, Box.marker); same(kind == 1 ? marker : null, Box.observed);
        same(null, Box.last);
        if (kind != 1 && Box.lastInput != 77) throw new AssertionError("factory ran after earlier failure");
    }
    private static void each(int route, boolean stop) throws Throwable {
        int[] marker = {13}; reset(marker);
        Core core = new Child(new long[]{5, 6}); core.marker = marker;
        ArrayList<Box> seen = new ArrayList<Box>();
        RuntimeException callback = new RuntimeException("owned-callback");
        Consumer<Box> action = box -> {
            same(Box.last, box);
            seen.add(box);
            if (box.value != (seen.size() == 1 ? 5 : 6)) throw new AssertionError("default callback bits changed");
            values++;
            if (stop) throw callback;
        };
        try {
            if (route == 0) ((Api<Box>) (Api) core).forEachRemaining(action);
            else EACH.invoke(core, action);
            if (stop) throw new AssertionError("callback failure missing");
        } catch (InvocationTargetException error) {
            same(callback, error.getCause()); failures++;
        } catch (RuntimeException error) {
            same(callback, error); failures++;
        }
        int count = stop ? 1 : 2;
        state(count, seen.size()); state(count, core.calls); state(count, core.index);
        state(count, Box.calls); state(13 + 8 * count, marker[0]);
        same(marker, core.observed); same(marker, Box.observed);
        same(marker, core.marker); same(marker, Box.marker);
        Object echo = new Object(); same(echo, core.echo(echo));
        try { core.remove(); throw new AssertionError("remove accepted"); }
        catch (UnsupportedOperationException expected) { failures++; }
        try {
            ((Api) core).forEachRemaining(null);
            throw new AssertionError("null action accepted");
        } catch (NullPointerException expected) { failures++; }
        state(count, core.calls); state(count, Box.calls);
    }
    public static void main(String[] args) throws Throwable {
        metadata(args[0].equals("original"));
        long[] edges = {Long.MIN_VALUE, -1, 0, 1, Long.MAX_VALUE};
        for (int route = 0; route < 8; route++) {
            for (long edge : edges) for (int mode = 0; mode < 3; mode++) success(route, edge, mode);
            for (int kind = 0; kind < 4; kind++) failed(route, kind);
        }
        for (int route = 0; route < 2; route++) { each(route, false); each(route, true); }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("original-scalar-target-and-factory=retained,mutable-factory-results-and-default-callback-order=retained,object-interface-argument-bridge-flags-and-final-typed-name=reported");
    }
}
