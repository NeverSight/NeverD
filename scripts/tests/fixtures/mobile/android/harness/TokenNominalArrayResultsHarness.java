import fixture.Box;
import fixture.Core;
import fixture.Trace;

public final class TokenNominalArrayResultsHarness {
    private static final class DerivedBox extends Box<String> {
        DerivedBox(String value) { super(value); }
    }

    private static void check(boolean condition, String label) {
        if (!condition) throw new AssertionError("owned token nominal array mismatch: " + label);
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    private static void attempt(final int route, final int mode, final int attempt) {
        final Box<?> first = mode == 1 || mode == 17 && attempt == 0
                ? new Box<Integer>(37) : new Box<String>(new String("initial"));
        final Object initial = mode == 2 ? null : mode == 3 ? new Box<?>[0]
                : mode == 4 ? new Box<?>[]{null} : mode == 9 ? new int[]{1}
                : mode == 10 ? new Object[]{first}
                : mode == 14 ? new DerivedBox[]{new DerivedBox("derived")}
                : new Box<?>[]{first};
        final Box<?> originalFirst = initial instanceof Box[] && ((Box<?>[]) initial).length != 0
                ? ((Box<?>[]) initial)[0] : null;
        final Box<?> replacement = mode == 15 ? null : new Box<String>(new String("replacement"));
        final Box<?> mutated = new Box<String>(new String("mutated"));
        final Box<?> callbackFirst = new Box<String>(new String("callback"));
        final Box<?>[] replacementArray = new Box<?>[]{callbackFirst};
        final Object initialValue = originalFirst == null ? null : originalFirst.value;
        final Object replacementValue = replacement == null ? null : replacement.value;
        final Object callbackValue = callbackFirst.value;
        final Object mutatedValue = mutated.value;
        final Object badArray = new Object();
        final Class<?> token = mode == 8 ? null : mode == 15 ? Integer.class : String.class;
        final RuntimeException callbackFailure = new IllegalStateException("callback");
        final Error callbackError = new LinkageError("callback");
        final Runnable action = mode == 5 ? null : new Runnable() {
            public void run() {
                Trace.callbacks++;
                if (mode == 6) throw callbackFailure;
                if (mode == 7) throw callbackError;
                if (mode == 11) Trace.array = replacementArray;
                if (mode == 12) Trace.array = null;
                if (mode == 13) ((Box<?>[]) initial)[0] = mutated;
                if (mode == 16) Trace.array = badArray;
            }
        };
        Trace.array = initial;
        Trace.token = null;
        Trace.name = "unwritten";
        Trace.action = action;
        Trace.calls = Trace.callbacks = Trace.stage = 0;
        final Object ignored = new Object();
        Object result = ignored;
        Throwable thrown = null;
        try {
            switch (route) {
            case 0: result = Core.raw(token); break;
            case 1: result = Core.objects(token); break;
            case 2: Core.ignore(token); break;
            case 3: result = Core.relay((Class) token); break;
            case 4: result = Core.stringValue((Class) token); break;
            case 5: result = Core.overwrite(token, replacement); break;
            default: throw new AssertionError("route");
            }
        } catch (Throwable failure) {
            thrown = failure;
        }
        final Object expectedArray = mode == 11 ? replacementArray : mode == 12 ? null
                : mode == 16 ? badArray : initial;
        final int stage = mode == 8 ? 1 : mode == 5 || mode == 6 || mode == 7 ? 2 : 3;
        final int callbacks = mode == 5 || mode == 8 ? 0 : 1;
        Box<?> expectedFirst = mode == 11 ? callbackFirst : mode == 13 ? mutated : originalFirst;
        Object expectedValue = mode == 11 ? callbackValue : mode == 13 ? mutatedValue : initialValue;
        Class<?> failure = mode == 5 || mode == 8 ? NullPointerException.class
                : mode == 6 ? IllegalStateException.class : mode == 7 ? LinkageError.class
                : expectedArray != null && !(expectedArray instanceof Box[]) ? ClassCastException.class : null;
        Object wanted = expectedArray;
        if (failure == null && (route == 4 || route == 5)) {
            Box<?>[] array = (Box<?>[]) expectedArray;
            if (array == null) failure = NullPointerException.class;
            else if (array.length == 0) failure = ArrayIndexOutOfBoundsException.class;
            else if (route == 4) {
                if (expectedFirst == null) failure = NullPointerException.class;
                else if (expectedValue != null && !(expectedValue instanceof String)) failure = ClassCastException.class;
                else wanted = expectedValue;
            } else if (mode == 14) failure = ArrayStoreException.class;
        }
        check(Trace.calls == 1 && Trace.callbacks == callbacks && Trace.stage == stage, "ordered effects");
        check(Trace.token == token && Trace.action == action && Trace.array == expectedArray, "original identities");
        check((mode == 8 ? "unwritten" : token.getName()).equals(Trace.name), "token read");
        check(failure == null ? thrown == null : thrown != null && thrown.getClass() == failure, "failure kind");
        if (mode == 6) check(thrown == callbackFailure, "exception identity");
        if (mode == 7) check(thrown == callbackError, "error identity");
        check(result == (failure == null && route != 2 ? wanted : ignored), "actual result and late cast");
        if (expectedArray instanceof Box[] && ((Box<?>[]) expectedArray).length != 0) {
            if (route == 5 && failure == null) {
                expectedFirst = replacement;
                expectedValue = replacementValue;
            }
            check(((Box<?>[]) expectedArray)[0] == expectedFirst, "original array store");
            if (expectedFirst != null) check(expectedFirst.value == expectedValue, "unchanged element value");
        }
        if ((mode == 11 || mode == 12 || mode == 16) && initial instanceof Box[])
            check(((Box<?>[]) initial)[0] == originalFirst, "retained array alias");
    }

    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; attempt++) attempt(route, mode, attempt);
        System.out.println("attempts=2,token-nominal-array-results=matched");
    }
}
