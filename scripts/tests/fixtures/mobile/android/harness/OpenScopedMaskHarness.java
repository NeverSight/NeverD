import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import kotlin.jvm.internal.DefaultConstructorMarker;
public class OpenScopedMaskHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("owned open scoped mask"); }
    static Span<?> make(int route, float start, float end, Object data, int mask) throws Throwable {
        if (route == 1) return OpenScopedMaskCaller.make(start, end, data, mask);
        if (route == 0) {
            try { return (Span<?>)Span.class.getDeclaredConstructor(float.class, float.class, Object.class,
                    int.class, DefaultConstructorMarker.class).newInstance(start, end, data, mask, null); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        MethodHandle handle = MethodHandles.publicLookup().findConstructor(Span.class,
                MethodType.methodType(void.class, float.class, float.class, Object.class, int.class, DefaultConstructorMarker.class));
        if (route == 2) return (Span<?>)handle.invokeWithArguments(start, end, data, mask, null);
        return (Span<?>) (Span)handle.invokeExact(start, end, data, mask, (DefaultConstructorMarker)null);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        boolean synthetic = Boolean.parseBoolean(args[2]);
        TypeVariable<?> formal = Span.class.getTypeParameters()[0];
        require(!Modifier.isFinal(Span.class.getModifiers()) && formal.getGenericDeclaration() == Span.class);
        require(Span.class.getField("data").getGenericType().equals(formal));
        require(Span.class.getDeclaredConstructor(float.class, float.class, Object.class)
                .getGenericParameterTypes()[2].equals(formal));
        Constructor<?> entry = Span.class.getDeclaredConstructor(float.class, float.class, Object.class,
                int.class, DefaultConstructorMarker.class);
        require(entry.isSynthetic() == synthetic && entry.getGenericParameterTypes()[2] == Object.class);
        int scenario = mode / 4;
        int[] masks = {0, 1, 4, -1};
        int[] starts = {0x80000000, 0x7fc01234, 0x7f800000, 0xff800000, 0x00800000, 0x3f000001};
        int[] ends = {0x7fc04321, 0x80000000, 0x3f800001, 0x00000001, 0xff800000, 0x7f800000};
        float start = Float.intBitsToFloat(starts[scenario]), end = Float.intBitsToFloat(ends[scenario]);
        int mask = masks[mode % 4];
        Object payload = scenario % 3 == 0 ? new Object() : scenario % 3 == 1 ? new Object[]{new Object()} : null;
        Object expected = (mask & 4) != 0 ? null : payload;
        Trace.fail = scenario < 2 ? 0 : scenario < 4 ? 1 : 2;
        Trace.failure = scenario % 2 == 0 ? new IllegalStateException("owned runtime") : new AssertionError("owned error");
        for (int attempt = 0; attempt < 2; ++attempt) {
            Trace.events = 0; Trace.partial = null; Trace.seen = new Object();
            Object untouched = Trace.seen;
            Span<?> result = null; Throwable failure = null;
            try { result = make(route, start, end, payload, mask); }
            catch (Throwable error) { failure = error; }
            long events = Trace.fail == 1 ? 1 : 12;
            require(Trace.events == events && Trace.partial instanceof Span);
            Span<?> partial = (Span<?>)Trace.partial;
            require(Float.floatToRawIntBits(partial.start) == starts[scenario]);
            require(Float.floatToRawIntBits(partial.end) == ends[scenario]);
            require(partial.data == expected && Trace.seen == (Trace.fail == 1 ? untouched : expected));
            if (Trace.fail == 0) require(failure == null && result == partial);
            else require(result == null && failure == Trace.failure);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + Trace.events);
        }
    }
}
