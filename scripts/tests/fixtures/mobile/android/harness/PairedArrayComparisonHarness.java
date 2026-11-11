import fixture.Core;
import fixture.Trace;
import java.util.Arrays;

public final class PairedArrayComparisonHarness {
    private static final RuntimeException RUNTIME = new RuntimeException("owned paired array failure");
    private static final Error ERROR = new AssertionError("owned paired array error");

    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned paired array comparison mismatch");
    }

    private static void attempt(final int route, final int mode) {
        final boolean strings = route == 3 || mode == 6 || mode == 16;
        Object[] left = strings ? new String[]{"left", "tail"} : new Object[]{new Object(), new Object()};
        Object[] right = strings ? new String[]{"right", "last"} : new Object[]{new Object(), new Object()};
        Object saved = strings ? "saved" : new Object();
        if (mode == 1) left = null;
        if (mode == 2 || mode == 16) right = null;
        if (mode == 3) left = strings ? new String[0] : new Object[0];
        if (mode == 4) right = strings ? new String[0] : new Object[0];
        if (mode == 5) right = left;
        if (mode == 6) saved = Integer.valueOf(19);
        if (mode == 11) left = right = null;
        if (mode == 17) { left = strings ? new String[0] : new Object[0]; right = strings ? new String[0] : new Object[0]; }
        if (mode == 18 && route != 3) {
            left = new Class<?>[]{String.class, int.class};
            right = new Class<?>[]{Integer.class, void.class};
            saved = Double.class;
        }
        final Object[] a = left, b = right;
        final Object value = saved;
        final Object[] oldA = a == null ? null : a.clone(), oldB = b == null ? null : b.clone();
        final Object replacement = strings ? "callback" : new Object();
        final Object marker = new Object(), otherMarker = new Object();
        final Object[] otherArray = new Object[]{new Object()};
        final int[] actions = new int[1];
        Trace.targets = Trace.forwards = Trace.stage = 0;
        Trace.left = Trace.right = null;
        Trace.leftValue = Trace.rightValue = Trace.seed = null;
        Trace.value = value;
        Trace.inspect = mode != 11 && mode != 16 && mode != 17;
        Trace.write = mode == 0 || mode == 5 || mode == 6 || mode == 7 || mode == 12
                || mode == 13 || mode == 14 || mode == 16 || mode == 18;
        Trace.index = mode == 7 ? 4 : 0;
        Trace.answer = (mode & 1) == 0;
        Trace.marker = marker;
        Trace.action = mode == 10 ? null : new Runnable() {
            public void run() {
                require(Trace.targets == 1 && Trace.forwards == (route == 1 ? 1 : 0));
                require(Trace.left == a && Trace.right == b && Trace.seed == value);
                require(Trace.stage == (Trace.inspect ? 3 : 1));
                if (Trace.inspect) require(Trace.leftValue == oldA[0] && Trace.rightValue == oldB[0]);
                actions[0]++;
                if (mode == 8) throw RUNTIME;
                if (mode == 9) throw ERROR;
                if (mode == 19) throw (RuntimeException)null;
                if (mode == 12) Trace.value = replacement;
                if (mode == 13) a[0] = replacement;
                if (mode == 14) Trace.left = otherArray;
                if (mode == 15) Trace.answer = !Trace.answer;
                Trace.marker = otherMarker;
            }
        };
        Object result = null;
        Throwable failure = null;
        try {
            result = route == 0 ? Boolean.valueOf(Core.variable(a, b)) : route == 1 ? Boolean.valueOf(Core.fixed(a, b))
                    : route == 2 ? Core.ignored(a, b) : Boolean.valueOf(Core.typed((String[])a, (String[])b));
        } catch (Throwable caught) { failure = caught; }
        boolean earlyLeft = mode == 1 || mode == 3, earlyRight = mode == 2 || mode == 4;
        boolean callbackFailure = mode == 8 || mode == 9 || mode == 10 || mode == 19;
        boolean storeFailure = mode == 6 || mode == 7;
        int stage = earlyLeft ? 1 : earlyRight ? 2 : callbackFailure ? 3 : storeFailure ? 4 : Trace.write ? 5 : 4;
        require(Trace.targets == 1 && Trace.forwards == (route == 1 ? 1 : 0) && Trace.stage == stage);
        require(Trace.left == (mode == 14 ? otherArray : a) && Trace.right == b && Trace.seed == value);
        require(actions[0] == (earlyLeft || earlyRight || mode == 10 ? 0 : 1));
        require(Trace.leftValue == (!Trace.inspect || earlyLeft ? null : oldA[0]));
        require(Trace.rightValue == (!Trace.inspect || earlyLeft || earlyRight ? null : oldB[0]));
        if (mode == 8) require(failure == RUNTIME);
        else if (mode == 9) require(failure == ERROR);
        else if (mode == 1 || mode == 2 || mode == 10 || mode == 19) require(failure instanceof NullPointerException);
        else if (mode == 3 || mode == 4 || mode == 7) require(failure instanceof ArrayIndexOutOfBoundsException);
        else if (mode == 6) require(failure instanceof ArrayStoreException);
        else {
            require(failure == null);
            require(result == (route == 2 ? otherMarker : Boolean.valueOf(mode == 15 ? true : (mode & 1) == 0)));
        }
        if (a != null) {
            Object[] expected = oldA.clone();
            if (Trace.write && !earlyLeft && !earlyRight && !callbackFailure && !storeFailure) expected[0] = value;
            require(Arrays.equals(a, expected));
        }
        if (b != null) {
            Object[] expected = b == a ? a : oldB;
            require(Arrays.equals(b, expected));
        }
        require(Trace.value == (mode == 12 ? replacement : value));
        require(Trace.marker == (earlyLeft || earlyRight || callbackFailure ? marker : otherMarker));
    }

    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int i = 0; i < 2; i++) attempt(route, mode);
        System.out.println("attempts=2,paired-array-comparison=matched");
    }
}
