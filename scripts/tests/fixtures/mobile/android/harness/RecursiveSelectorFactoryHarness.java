import fixture.Core;
import fixture.Trace;
import java.util.Comparator;
import java.util.function.Function;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class RecursiveSelectorFactoryHarness {
    private static void require(boolean value) {
        if (!value) throw new AssertionError("owned recursive selector factory mismatch");
    }
    private static Comparator call(int route, Function selector) {
        switch (route) {
            case 0: return Core.raw(selector);
            case 1: Core.ignore(selector); return null;
            case 2: return Core.relay(selector);
            case 3: return Core.standard(selector);
            case 4: Core.ignoreStandard(selector); return null;
            default: throw new AssertionError();
        }
    }
    private static void check(int route, final int mode, int attempt) {
        final boolean custom = route < 3, ignored = route == 1 || route == 4;
        final Object input = mode == 13 ? null : new Object();
        final Object left = new Object(), right = new Object(), poison = new Object();
        final RuntimeException runtime = new IllegalArgumentException("callback");
        final Error error = new AssertionError("callback");
        final Comparator reply = new Comparator() {
            public int compare(Object a, Object b) {
                require(a == left && b == right); Trace.outputComparisons++; return -17;
            }
        };
        final Comparator late = new Comparator() {
            public int compare(Object a, Object b) {
                require(a == left && b == right); Trace.outputComparisons++; return 23;
            }
        };
        class Key implements Comparable<Object> {
            public int compareTo(Object other) {
                Trace.keyComparisons++;
                Trace.comparedReceiver = this; Trace.comparedArgument = other;
                if (mode == 6) throw runtime;
                if (mode == 7) throw error;
                if (mode == 9) Trace.result = late;
                if (mode == 15) Trace.result = null;
                return this == other ? 0 : -9;
            }
        }
        final Key first = new Key(), second = new Key();
        final Object key = mode == 2 ? poison : mode == 3 ? null : mode == 17 ? "key" : first;
        Trace.calls = Trace.stage = Trace.callbacks = Trace.keyComparisons = Trace.outputComparisons = Trace.comparison = 0;
        Trace.input = input; Trace.seenInput = Trace.key = Trace.firstInput = Trace.lastInput = null;
        Trace.comparedReceiver = Trace.comparedArgument = null;
        Trace.result = mode == 10 ? null : mode == 11 ? poison : reply;
        Trace.failInitialization = mode == 12; Trace.initializationFailure = runtime;
        final Function function = new Function() {
            public Object apply(Object value) {
                Trace.callbacks++;
                if (Trace.callbacks == 1) Trace.firstInput = value;
                Trace.lastInput = value;
                require(custom ? value == input && Trace.callbacks == 1
                    : value == (Trace.callbacks == 1 ? left : right));
                if (mode == 4) throw runtime;
                if (mode == 5) throw error;
                if (mode == 8) { Trace.input = poison; Trace.result = late; }
                if (mode == 14) Trace.result = poison;
                return !custom && Trace.callbacks == 2 && mode != 16 && mode != 17
                    ? second : key;
            }
        };
        Function selector = mode == 1 ? null : function;
        Comparator result = null; Throwable failure = null; boolean made = false;
        int value = 0;
        try {
            result = call(route, selector); made = true;
            if (!custom) require(Trace.callbacks == 0 && Trace.keyComparisons == 0);
            if (!ignored && result != null) value = result.compare(left, right);
        } catch (Throwable caught) { failure = caught; }
        require(Trace.initializations == (custom ? 1 : 0));
        if (custom && mode == 12) {
            require(Trace.calls == 0 && Trace.stage == 0 && Trace.callbacks == 0 && !made);
            if (attempt == 0) require(failure instanceof ExceptionInInitializerError
                && failure.getCause() == runtime);
            else require(failure instanceof NoClassDefFoundError);
        } else if (custom) {
            require(Trace.calls == 1 && Trace.seenInput == input);
            int stage = mode == 1 || mode == 2 || mode == 4 || mode == 5 ? 1
                : mode == 3 || mode == 6 || mode == 7 ? 2 : 3;
            require(Trace.stage == stage && Trace.callbacks == (mode == 1 ? 0 : 1));
            require(Trace.key == (stage < 2 ? null : key));
            int comparisons = stage < 2 || mode == 3 || mode == 17 ? 0 : 1;
            require(Trace.keyComparisons == comparisons);
            if (comparisons != 0) require(Trace.comparedReceiver == first && Trace.comparedArgument == first);
            if (mode == 1 || mode == 3) require(failure instanceof NullPointerException && !made);
            else if (mode == 2 || mode == 11 || mode == 14) require(failure instanceof ClassCastException && !made);
            else if (mode == 4 || mode == 6) require(failure == runtime && !made);
            else if (mode == 5 || mode == 7) require(failure == error && !made);
            else {
                Comparator expected = mode == 10 || mode == 15 ? null : mode == 8 || mode == 9 ? late : reply;
                require(failure == null && made && result == (ignored ? null : expected));
                require(Trace.comparison == 0);
                require(value == (ignored || expected == null ? 0 : expected == late ? 23 : -17));
            }
            require(Trace.outputComparisons == (made && !ignored && result != null ? 1 : 0));
        } else {
            require(Trace.calls == 0 && Trace.stage == 0 && Trace.seenInput == null && Trace.key == null
                && Trace.outputComparisons == 0);
            if (mode == 1) require(failure instanceof NullPointerException && !made && Trace.callbacks == 0);
            else if (ignored) require(failure == null && made && Trace.callbacks == 0 && Trace.keyComparisons == 0);
            else {
                require(made && result != null && Trace.firstInput == left);
                if (mode == 2) require(failure instanceof ClassCastException && Trace.callbacks == 1 && Trace.keyComparisons == 0);
                else if (mode == 3) require(failure instanceof NullPointerException && Trace.callbacks >= 1 && Trace.callbacks <= 2 && Trace.keyComparisons == 0);
                else if (mode == 4) require(failure == runtime && Trace.callbacks == 1 && Trace.keyComparisons == 0);
                else if (mode == 5) require(failure == error && Trace.callbacks == 1 && Trace.keyComparisons == 0);
                else if (mode == 6) require(failure == runtime && Trace.callbacks == 2 && Trace.keyComparisons == 1);
                else if (mode == 7) require(failure == error && Trace.callbacks == 2 && Trace.keyComparisons == 1);
                else {
                    require(failure == null && Trace.callbacks == 2 && Trace.lastInput == right);
                    require(Trace.keyComparisons == (mode == 17 ? 0 : 1));
                    if (mode != 17) require(Trace.comparedReceiver == first && Trace.comparedArgument == (mode == 16 ? first : second));
                    require(value == (mode == 16 || mode == 17 ? 0 : -9));
                }
            }
        }
        int last = Trace.lastInput == left ? 1 : Trace.lastInput == right ? 2 : Trace.lastInput == null ? 0 : 3;
        System.out.println("attempt=" + attempt + ",callbacks=" + Trace.callbacks + ",keys=" + Trace.keyComparisons
            + ",output=" + Trace.outputComparisons + ",stage=" + Trace.stage + ",factory=" + Trace.calls
            + ",last=" + last + ",value=" + value);
    }
    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        check(route, mode, 0); check(route, mode, 1);
    }
}
