import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.*;
public final class ClosedArraySuperHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("closed array super constructor changed"); }
    static Child create(int route, Object capture, Object context, int index, CharSequence[] input, long bits) throws Throwable {
        if (route == 0) return new Child(capture, context, index, input, bits);
        if (route == 1) return ClosedArraySuperCaller.create(capture, context, index, input, bits);
        if (route == 2) {
            try { return Child.class.getConstructor(Object.class, Object.class, int.class, CharSequence[].class, long.class)
                .newInstance(capture, context, index, input, bits); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Child) MethodHandles.publicLookup().findConstructor(Child.class,
            MethodType.methodType(void.class, Object.class, Object.class, int.class, CharSequence[].class, long.class))
            .invokeExact(capture, context, index, input, bits);
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        Object capture = mode == 13 ? null : new Object(), context = new Object();
        long bits = (mode & 1) == 0 ? 0x8123456789abcdefL : 0x7ff8000000000017L;
        int index = mode == 12 ? 2 : 0;
        Trace.replacement = mode == 1 ? new StringBuilder("replacement") : mode == 2 ? new Object()
            : mode == 3 ? null : new String("replacement");
        Trace.failure = new IllegalStateException("owned array constructor failure");
        int[] failures = {0, 0, 0, 0, 1, 4, 7, 2, 6, 8, 3, 5, 0, 0};
        Trace.failAt = failures[mode];
        Set<Integer> initialized = new HashSet<Integer>(), failed = new HashSet<Integer>();
        require(Modifier.isFinal(Child.class.getField("capture").getModifiers()));
        for (int attempt = 0; attempt < 2; attempt++) {
            CharSequence original = new String("original");
            CharSequence[] input = variant == 0 ? new CharSequence[]{original} : variant == 1 ? new String[]{(String) original}
                : variant == 2 ? null : new CharSequence[0];
            Trace.value = 0; Trace.seen = null; Trace.captureAt = null; Trace.markerAt = -1;
            Child result = null; Throwable failure = null;
            try { result = create(route, capture, context, index, input, bits); }
            catch (Throwable problem) { failure = problem; }
            long trace = 0; int failureKind = 0, marker = 0;
            boolean escaped = false, stored = false;
            for (int stage : new int[]{1, 4, 7, 2, 6, 8, -1, 3, 5}) {
                if (stage == -1) {
                    if (input == null) failureKind = 4;
                    else if (index >= input.length) failureKind = 5;
                    else if (Trace.replacement != null && !input.getClass().getComponentType().isInstance(Trace.replacement)) failureKind = 6;
                    else stored = true;
                    if (failureKind != 0) break;
                    continue;
                }
                boolean init = stage == 1 || stage == 4;
                if (init && failed.contains(stage)) { failureKind = 3; break; }
                if (init && initialized.contains(stage)) continue;
                if (stage == 6) escaped = true;
                if (stage == 5) marker = 99;
                trace = trace * 17 + stage;
                if (Trace.failAt == stage) {
                    failureKind = init ? 2 : 1;
                    if (init) failed.add(stage);
                    break;
                }
                if (init) initialized.add(stage);
            }
            require(Trace.value == trace);
            if (escaped) require(Trace.seen != null && Trace.seen.context == context && Trace.seen.values == input
                && Trace.seen.count == 2 && Trace.seen.index == index && Trace.seen.bits == bits
                && Trace.seen.capture == capture && Trace.captureAt == capture && Trace.markerAt == 0 && Trace.seen.marker == marker);
            else require(Trace.seen == null && Trace.captureAt == null && Trace.markerAt == -1);
            if (input != null && input.length > 0) require(input[0] == (stored ? Trace.replacement : original));
            if (failureKind == 1) require(failure == Trace.failure);
            else if (failureKind == 2) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
            else if (failureKind == 3) require(failure instanceof NoClassDefFoundError);
            else if (failureKind == 4) require(failure instanceof NullPointerException);
            else if (failureKind == 5) require(failure instanceof ArrayIndexOutOfBoundsException);
            else if (failureKind == 6) require(failure instanceof ArrayStoreException);
            else require(failure == null && result == Trace.seen && result.marker == 99);
            if (failureKind != 0) require(result == null);
            System.out.println("variant=" + variant + ",route=" + route + ",mode=" + mode
                + ",attempt=" + attempt + ",trace=" + Trace.value);
        }
    }
}
