import fixture.Bucket;
import fixture.Core;
import fixture.Pair;
import fixture.Trace;
import java.util.Arrays;
import java.util.Collections;
import java.util.Map;
import java.util.function.Function;
import java.util.stream.Collector;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class PairedMapperCollectorHarness {
    private static final RuntimeException RUNTIME = new RuntimeException("owned mapper failure");
    private static final Error ERROR = new AssertionError("owned mapper error");

    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned paired mapper collector mismatch");
    }

    private static void owned(int route, final int mode) {
        final Object source = mode == 1 ? null : new Object();
        final Object replacement = new Object();
        final Object left = new Object(), right = new Object();
        final Pair pair = new Pair(left, right);
        final Bucket original = new Bucket(mode == 14 ? replacement : pair);
        final Bucket other = new Bucket(new Pair(right, left));
        final int[] count = new int[3];
        final Object[] received = new Object[2];
        Trace.targets = Trace.stage = 0;
        Trace.invoke = mode != 2 && mode != 19;
        Trace.source = source;
        Trace.seen = Trace.left = Trace.right = null;
        Trace.first = Trace.second = null;
        Trace.result = mode == 12 ? null : mode == 13 ? replacement : original;
        final Runnable action = new Runnable() {
            public void run() {
                require(Trace.targets == 1 && Trace.stage == (Trace.invoke ? 3 : 1));
                count[2]++;
                if (mode == 9) throw RUNTIME;
                if (mode == 10) throw ERROR;
                if (mode == 15) Trace.result = other;
                if (mode == 16) Trace.source = replacement;
            }
        };
        Trace.action = mode == 11 ? null : action;
        Function first = new Function() {
            public Object apply(Object input) {
                require(Trace.stage == 1 || mode == 18 && Trace.stage == 2);
                count[0]++;
                received[0] = input;
                if (mode == 5) throw RUNTIME;
                if (mode == 6) throw ERROR;
                if (mode == 17) {
                    Trace.source = replacement;
                    Trace.second = null;
                }
                return left;
            }
        };
        Function second = new Function() {
            public Object apply(Object input) {
                require(Trace.stage == 2 && count[0] == 1);
                count[1]++;
                received[1] = input;
                if (mode == 7) throw RUNTIME;
                if (mode == 8) throw ERROR;
                return right;
            }
        };
        if (mode == 2 || mode == 3 || mode == 19) first = null;
        if (mode == 2 || mode == 4 || mode == 19) second = null;
        if (mode == 18) second = first;
        Object result = null;
        Throwable failure = null;
        try {
            result = route == 0 ? Core.owned(first, second)
                    : route == 1 ? Core.checked(first, second)
                    : Core.ignored(first, second);
        } catch (Throwable caught) { failure = caught; }
        int firstCount = Trace.invoke && mode != 3 ? 1 : 0;
        int secondCount = Trace.invoke && mode != 3 && mode != 4 && mode != 5 && mode != 6 ? 1 : 0;
        int actionCount = mode == 3 || mode == 4 || mode == 5 || mode == 6 || mode == 7 || mode == 8 || mode == 11 ? 0 : 1;
        int stage = mode == 3 || mode == 5 || mode == 6 ? 1
                : mode == 4 || mode == 7 || mode == 8 ? 2
                : mode == 9 || mode == 10 || mode == 11 ? 3 : 4;
        require(Trace.targets == 1 && Trace.stage == stage && Trace.seen == source);
        require(Trace.first == first && Trace.second == (mode == 17 ? null : second));
        require(count[0] == firstCount + (mode == 18 ? 1 : 0));
        require(count[1] == (mode == 18 ? 0 : secondCount) && count[2] == actionCount);
        if (firstCount != 0) require(received[0] == source);
        if (secondCount != 0 && mode != 18) require(received[1] == source);
        require(Trace.left == (firstCount != 0 && mode != 5 && mode != 6 ? left : null));
        require(Trace.right == (secondCount != 0 && mode != 7 && mode != 8 ? mode == 18 ? left : right : null));
        if (mode == 5 || mode == 7 || mode == 9) require(failure == RUNTIME);
        else if (mode == 6 || mode == 8 || mode == 10) require(failure == ERROR);
        else if (mode == 3 || mode == 4 || mode == 11 || mode == 12 && route == 1)
            require(failure instanceof NullPointerException);
        else if (mode == 13 || mode == 14 && route == 1)
            require(failure instanceof ClassCastException);
        else {
            require(failure == null);
            Bucket selected = mode == 15 ? other : mode == 12 ? null : original;
            require(result == (route == 1 ? selected.value : selected));
        }
        require(Trace.source == (mode == 16 || mode == 17 ? replacement : source));
        require(Trace.result == (mode == 15 ? other : mode == 12 ? null : mode == 13 ? replacement : original));
        require(original.value == (mode == 14 ? replacement : pair));
        require(pair.left == left && pair.right == right);
    }

    private static void standard(final int mode) {
        final Object one = new Object(), two = new Object();
        final int[] count = new int[2];
        Function first = new Function() {
            public Object apply(Object input) {
                count[0]++;
                if (mode == 7) throw RUNTIME;
                if (mode == 8) throw ERROR;
                return mode == 4 ? one : mode == 5 ? null : input;
            }
        };
        Function second = new Function() {
            public Object apply(Object input) {
                count[1]++;
                if (mode == 9) throw RUNTIME;
                if (mode == 10) throw ERROR;
                return mode == 6 ? null : input == one ? two : one;
            }
        };
        if (mode == 2) first = null;
        if (mode == 3) second = null;
        if (mode == 12) second = first;
        Collector collector = null;
        Throwable failure = null;
        try { collector = Core.standard(first, second); }
        catch (Throwable caught) { failure = caught; }
        require(count[0] == 0 && count[1] == 0);
        if (mode == 2 || mode == 3) {
            if (failure == null) {
                require(collector != null);
                try { Collections.singletonList(one).stream().collect(collector); }
                catch (Throwable caught) { failure = caught; }
            }
            require(failure instanceof NullPointerException);
            return;
        }
        require(failure == null && collector != null);
        Map result = null;
        Object a = mode == 11 ? null : one;
        Object b = mode == 13 ? "different" : two;
        try {
            result = (Map)(mode == 0 ? Collections.emptyList() : mode == 5 ? Collections.singletonList(a) : Arrays.asList(a, b))
                    .stream().collect(collector);
        } catch (Throwable caught) { failure = caught; }
        if (mode == 7 || mode == 9) require(failure == RUNTIME);
        else if (mode == 8 || mode == 10) require(failure == ERROR);
        else if (mode == 4) require(failure instanceof IllegalStateException);
        else if (mode == 6) require(failure instanceof NullPointerException);
        else {
            require(failure == null && result.size() == (mode == 0 ? 0 : mode == 5 ? 1 : 2));
            if (mode != 0) {
                require(result.get(mode == 5 ? null : a) == (mode == 12 ? a : a == one ? two : one));
                if (mode != 5) require(result.get(b) == (mode == 12 ? b : one));
            }
            require(count[0] == (mode == 12 ? 4 : mode == 0 ? 0 : mode == 5 ? 1 : 2));
            require(count[1] == (mode == 12 ? 0 : mode == 0 ? 0 : mode == 5 ? 1 : 2));
        }
    }

    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; attempt++) {
            if (route == 3) standard(mode); else owned(route, mode);
        }
        System.out.println("attempts=2,paired-mapper-collector=matched");
    }
}
