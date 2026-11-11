import fixture.Api;
import fixture.ByteBox;
import fixture.Carrier;
import fixture.Host;
import fixture.LongBox;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;

public final class LocalNominalScalarGuardsHarness {
    private static int values, identities, states, failures;
    private static void check(boolean condition) {
        ++values;
        if (!condition) throw new AssertionError("scalar value " + values);
    }
    private static void same(Object actual, Object expected) {
        ++identities;
        if (actual != expected) throw new AssertionError("scalar identity " + identities);
    }
    private static Object call(int route, Api<String> view, String name, Object input) throws Throwable {
        if (route == 0 || route == 1) {
            Api<String> alias = view;
            return name.equals("contains") ? alias.contains(input) : alias.index(input);
        }
        if (route == 2 || route == 3) {
            Method method = (route == 2 ? Api.class : view.getClass()).getMethod(name, Object.class);
            method.setAccessible(true);
            try { return method.invoke(view, input); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        Class<?> owner = route == 4 ? Api.class : view.getClass();
        MethodHandles.Lookup lookup = route == 4 ? MethodHandles.publicLookup()
                : MethodHandles.privateLookupIn(owner, MethodHandles.lookup());
        return lookup.findVirtual(owner, name, MethodType.methodType(
                name.equals("contains") ? boolean.class : int.class, Object.class)).invoke(view, input);
    }
    private static void state(Trace trace, Carrier box, int calls, int marker,
                              int boxCalls, int boxMarker) {
        ++states;
        if (trace.calls != calls || trace.marker != marker || box.calls != boxCalls
                || box.marker[0] != boxMarker) throw new AssertionError("scalar state " + states);
    }
    private static void exercise(int route, String name, long input) throws Throwable {
        Trace trace = new Trace();
        trace.expectedByte = (byte) input;
        trace.expectedLong = input;
        Api<String> view = Host.create(trace), alias = Host.create(trace);
        Carrier box = name.equals("contains") ? new ByteBox((byte) input) : new LongBox(input);
        box.marker = new int[]{7};
        int calls = 0, marker = 0, boxCalls = 0, boxMarker = 7;
        check(call(route, view, name, box).equals(name.equals("contains") ? Boolean.TRUE : Integer.valueOf(13)));
        marker = marker * 5 + (name.equals("contains") ? 1 : 2);
        state(trace, box, ++calls, marker, ++boxCalls, boxMarker += 3);
        check(name.equals("contains") ? trace.lastByte == (byte) input : trace.lastLong == input);
        if (name.equals("contains")) box.byteValue ^= 1;
        else box.longValue ^= 1L;
        check(call(route, alias, name, box).equals(name.equals("contains") ? Boolean.FALSE : Integer.valueOf(-3)));
        marker = marker * 5 + (name.equals("contains") ? 1 : 2);
        state(trace, box, ++calls, marker, ++boxCalls, boxMarker += 3);
        Object token = new Object();
        same(view.echo(token, alias), token);
        same(view.echo(null, token), null);
        Object[] array = {token};
        same(view.echo(array, token), array);
        array[0] = alias;
        same(view.echo(array, trace)[0], alias);
        RuntimeException dormant = new IllegalStateException("dormant");
        box.failure = dormant;
        trace.failure = dormant;
        for (Object wrong : new Object[]{null, new Object(), "wrong", new Carrier((byte) input, input),
                name.equals("contains") ? new LongBox(input) : new ByteBox((byte) input)}) {
            check(call(route, view, name, wrong).equals(name.equals("contains") ? Boolean.FALSE : Integer.valueOf(-1)));
            state(trace, box, calls, marker, boxCalls, boxMarker);
        }
        try { call(route, view, name, box); throw new AssertionError("missing unbox failure"); }
        catch (IllegalStateException failure) { same(failure, dormant); ++failures; }
        state(trace, box, calls, marker, ++boxCalls, boxMarker += 3);
        box.failure = null;
        try { call(route, view, name, box); throw new AssertionError("missing target failure"); }
        catch (IllegalStateException failure) { same(failure, dormant); ++failures; }
        marker = marker * 5 + (name.equals("contains") ? 1 : 2);
        state(trace, box, ++calls, marker, ++boxCalls, boxMarker += 3);
        check(name.equals("contains") ? trace.lastByte == box.byteValue : trace.lastLong == box.longValue);
        trace.failure = null;
        check(call(route, alias, name, box).equals(name.equals("contains") ? Boolean.FALSE : Integer.valueOf(-3)));
        marker = marker * 5 + (name.equals("contains") ? 1 : 2);
        state(trace, box, ++calls, marker, ++boxCalls, boxMarker += 3);
        check(view.getClass().getEnclosingMethod().getName().equals("create"));
        check(Modifier.isStatic(view.getClass().getEnclosingMethod().getModifiers()));
        check(view.getClass().getTypeParameters().length == 0);
        check(view.getClass().getMethod("echo", Object.class, Object.class).getTypeParameters().length == 2);
    }
    public static void main(String[] arguments) throws Throwable {
        for (int route = 0; route < 6; ++route) {
            for (byte value : new byte[]{Byte.MIN_VALUE, -1, 0, 1, Byte.MAX_VALUE})
                exercise(route, "contains", value);
            for (long value : new long[]{Long.MIN_VALUE, -1L, 0L, 1L, Long.MAX_VALUE})
                exercise(route, "index", value);
        }
        System.out.println("values=" + values + ",identities=" + identities
                + ",states=" + states + ",failures=" + failures);
        System.out.println("nominal-unboxing=original,local-scope=closed,aliases-and-failures=ordered");
    }
}
