import fixture.Api;
import fixture.Box;
import fixture.Core;
import fixture.Marker;
import fixture.Base;
import fixture.Parent;
import fixture.Caller;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;
import java.util.ArrayList;
import java.util.NoSuchElementException;
import java.util.function.Consumer;

public final class BoxedSuperclassResultsHarness {
    private static int values, identities, states, failures;
    private static final Method NEXT, ROOT, EACH;
    private static final MethodHandle HANDLE;
    static {
        try {
            NEXT = Core.class.getMethod("get", int.class);
            ROOT = Base.class.getMethod("get", int.class);
            EACH = Api.class.getMethod("each", int.class, Consumer.class);
            HANDLE = MethodHandles.lookup().findVirtual(Parent.class, "get",
                MethodType.methodType(Object.class, int.class));
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
    private static Object call(int route, Core core, int index) throws Throwable {
        try {
            switch (route) {
                case 0: return core.get(index);
                case 1: return ((Api) core).get(index);
                case 2: return ((Api<Box>) (Api) core).get(index);
                case 3: return NEXT.invoke(core, index);
                case 4: return ROOT.invoke(core, index);
                case 5: return (Object) HANDLE.invokeExact((Parent) core, index);
                case 6: return ((Api<Box>) (Api) core).at(index);
                case 7: return ((Base<Box>) (Base) core).parentGet(index);
                case 8: return Caller.call(core, index);
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
        ParameterizedType edge = (ParameterizedType) Core.class.getGenericSuperclass();
        if (edge.getRawType() != Parent.class || edge.getActualTypeArguments()[0] != (original ? Box.class : Object.class)
                || Core.class.getInterfaces()[0] != Marker.class
                || NEXT.getReturnType() != Object.class || NEXT.getGenericReturnType() != Object.class
                || NEXT.isBridge() != original || NEXT.isSynthetic() != original
                || !ROOT.getGenericReturnType().equals(Base.class.getTypeParameters()[0])
                || !Core.class.getMethod("echo", Object.class).getGenericReturnType()
                    .equals(Core.class.getMethod("echo", Object.class).getTypeParameters()[0]))
            throw new AssertionError("complete interface, runtime entry or independent method scope changed");
    }
    private static void success(int route, long input, int mode) throws Throwable {
        int[] marker = {7};
        reset(marker);
        Core core = new Core(new long[]{77, input});
        core.marker = marker;
        Box cached = Box.box(input);
        reset(marker);
        marker[0] = 7;
        if (mode == 1) Box.cached = cached;
        if (mode == 2) Box.returnNull = true;
        Object result = call(route, core, 1);
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
        Core core = new Core(new long[]{Long.MIN_VALUE}); core.marker = marker;
        RuntimeException target = new RuntimeException("owned-target");
        RuntimeException factory = new RuntimeException("owned-factory");
        if (kind == 0) core.failure = target;
        if (kind == 1) Box.failure = factory;
        if (kind == 2) core.index = 1;
        try { call(route, kind == 3 ? null : core, kind == 2 ? 1 : 0); throw new AssertionError("failure missing"); }
        catch (Throwable error) {
            if (error instanceof AssertionError) throw error;
            if (kind == 0 || kind == 1) same(kind == 0 ? target : factory, error);
            else if (kind == 2 ? !(error instanceof ArrayIndexOutOfBoundsException) : !(error instanceof NullPointerException))
                throw new AssertionError("failure order changed", error);
            failures++;
        }
        state(kind == 3 ? 0 : 1, core.calls);
        state(kind == 2 ? 1 : 0, core.index);
        state(kind == 1 ? 1 : 0, Box.calls);
        state(11 + (kind == 3 ? 0 : kind == 1 ? 8 : 3), marker[0]);
        same(marker, core.marker); same(kind == 3 ? null : marker, core.observed);
        same(marker, Box.marker); same(kind == 1 ? marker : null, Box.observed);
        same(null, Box.last);
        if (kind != 1 && Box.lastInput != 77) throw new AssertionError("factory ran after earlier failure");
    }
    private static void each(int route, boolean stop) throws Throwable {
        int[] marker = {13}; reset(marker);
        Core core = new Core(new long[]{5, 6}); core.marker = marker;
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
            if (route == 0) ((Api<Box>) (Api) core).each(0, action);
            else EACH.invoke(core, 0, action);
            if (stop) throw new AssertionError("callback failure missing");
        } catch (InvocationTargetException error) {
            same(callback, error.getCause()); failures++;
        } catch (RuntimeException error) {
            same(callback, error); failures++;
        }
        int count = stop ? 1 : 2;
        state(count, seen.size()); state(count, core.calls); state(count - 1, core.index);
        state(count, Box.calls); state(13 + 8 * count, marker[0]);
        same(marker, core.observed); same(marker, Box.observed);
        same(marker, core.marker); same(marker, Box.marker);
        Object echo = new Object(); same(echo, core.echo(echo));
        try {
            ((Api) core).each(0, null);
            throw new AssertionError("null action accepted");
        } catch (NullPointerException expected) { failures++; }
        state(count, core.calls); state(count, Box.calls);
    }
    private static void repeated(int route) throws Throwable {
        int[] marker = {3}; reset(marker);
        long[] input = {17, Long.MIN_VALUE};
        Core core = new Core(input); core.marker = marker;
        Box first = null;
        for (int pass = 0; pass < 3; pass++) {
            input[1] = Long.MIN_VALUE + pass;
            if (pass == 1) Box.cached = first;
            if (pass == 2) Box.returnNull = true;
            Object result = call(route, core, 1);
            if (pass == 0) first = (Box) result;
            if (Box.lastInput != input[1]) throw new AssertionError("mutable source array ignored");
            values++;
            if (pass == 2 ? result != null : ((Box) result).value != Long.MIN_VALUE)
                throw new AssertionError("factory cache or null result changed");
            values++;
            same(pass == 2 ? null : first, result); same(input, core.values);
            same(marker, core.observed); same(marker, Box.observed); same(marker, core.marker);
            state(pass + 1, core.calls); state(pass + 1, Box.calls);
            state(1, core.index); state(3 + 8 * (pass + 1), marker[0]);
        }
    }
    public static void main(String[] args) throws Throwable {
        metadata(args[0].equals("original"));
        long[] edges = {Long.MIN_VALUE, -1, 0, 1, Long.MAX_VALUE};
        for (int route = 0; route < 9; route++) {
            for (long edge : edges) for (int mode = 0; mode < 3; mode++) success(route, edge, mode);
            for (int kind = 0; kind < 4; kind++) failed(route, kind);
            repeated(route);
        }
        for (int route = 0; route < 2; route++) { each(route, false); each(route, true); }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("original-indexed-target-and-factory=retained,mutable-aliases-and-inherited-callback-order=retained,object-superclass-argument-and-bridge-flags=reported");
    }
}
