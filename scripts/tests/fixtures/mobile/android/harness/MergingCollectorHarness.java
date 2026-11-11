import fixture.Trace;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Map;
import java.util.LinkedHashMap;
import java.util.function.Function;
import java.util.function.BinaryOperator;
import java.util.function.Supplier;
import java.util.stream.Collector;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class MergingCollectorHarness {
    private static final RuntimeException RUNTIME = new RuntimeException("owned collector failure");
    private static final Error ERROR = new AssertionError("owned collector error");
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned merging collector mismatch");
    }
    private static Collector collector() {
        return Collector.of(ArrayList::new, ArrayList::add,
            (ArrayList a, ArrayList b) -> { a.addAll(b); return a; });
    }
    private static void owned(int route, final int mode, int attempt) {
        final Object wrong = new Object(), key = new Object();
        final Object source = mode == 20 ? null : mode == 22 ? wrong : new String("owned source");
        final String value = new String("owned value"), extra = new String("owned extra"), merged = new String("owned merged");
        final Map index = new LinkedHashMap(); index.put(key, value);
        final Collector original = collector(), other = collector();
        final int[] calls = new int[5];
        Trace.targets = 0; Trace.events = 0; Trace.source = source;
        Trace.seen = Trace.left = Trace.right = Trace.merged = null;
        Trace.first = Trace.second = Trace.operator = Trace.mapSupplier = null;
        Trace.extra = mode == 23 ? wrong : extra;
        Trace.map = null;
        Trace.result = mode == 17 ? wrong : mode == 18 ? null : original;
        Function first = mode == 1 ? null : new Function<String, Object>() {
            public Object apply(String input) {
                require(Trace.events == 1 && input == source); calls[0]++;
                if (mode == 2) throw RUNTIME;
                if (mode == 3) throw ERROR;
                if (mode == 21) { Trace.source = wrong; Trace.second = null; }
                return key;
            }
        };
        Function second = mode == 4 ? null : new Function<String, String>() {
            public String apply(String input) {
                require(Trace.events == 12 && input == source); calls[1]++;
                if (mode == 5) throw RUNTIME;
                if (mode == 6) throw ERROR;
                return value;
            }
        };
        BinaryOperator merger = mode == 7 ? null : new BinaryOperator<String>() {
            public String apply(String left, String right) {
                require(Trace.events == 123 && left == value && right == extra); calls[2]++;
                if (mode == 8) throw RUNTIME;
                if (mode == 9) throw ERROR;
                return merged;
            }
        };
        Supplier supplier = mode == 10 ? null : new Supplier() {
            public Object get() {
                require(Trace.events == 1234); calls[3]++;
                if (mode == 12) throw RUNTIME;
                if (mode == 13) throw ERROR;
                return mode == 11 ? wrong : index;
            }
        };
        Trace.action = mode == 14 ? null : new Runnable() {
            public void run() {
                require(Trace.events == 12345); calls[4]++;
                if (mode == 15) throw RUNTIME;
                if (mode == 16) throw ERROR;
                if (mode == 19) Trace.result = other;
            }
        };
        Object result = null; Throwable failure = null;
        try { result = MergingCollectorCaller.call(route, first, second, merger, supplier); }
        catch (Throwable caught) { failure = caught; }
        int stage = mode >= 1 && mode <= 3 || mode == 22 ? 1 : mode >= 4 && mode <= 6 ? 2
            : mode >= 7 && mode <= 9 || mode == 23 ? 3 : mode >= 10 && mode <= 13 ? 4
            : mode >= 14 && mode <= 16 ? 5 : 6;
        long events = 0;
        for (int digit = 1; digit <= stage; digit++) events = events * 10 + digit;
        require(Trace.targets == 1 && Trace.events == events && Trace.seen == source);
        require(Trace.first == first && Trace.second == (mode == 21 ? null : second));
        require(Trace.operator == merger && Trace.mapSupplier == supplier);
        require(Trace.source == (mode == 21 ? wrong : source));
        require(calls[0] == (mode == 1 || mode == 22 ? 0 : 1));
        require(calls[1] == (stage >= 2 && mode != 4 ? 1 : 0));
        require(calls[2] == (stage >= 3 && mode != 7 && mode != 23 ? 1 : 0));
        require(calls[3] == (stage >= 4 && mode != 10 ? 1 : 0));
        require(calls[4] == (stage >= 5 && mode != 14 ? 1 : 0));
        require(Trace.left == (stage >= 2 ? key : null) && Trace.right == (stage >= 3 ? value : null));
        require(Trace.merged == (stage >= 4 ? merged : null) && Trace.map == (stage >= 5 ? index : null));
        Object expected = mode == 17 ? wrong : mode == 18 ? null : mode == 19 ? other : original;
        require(Trace.result == expected);
        if (mode == 2 || mode == 5 || mode == 8 || mode == 12 || mode == 15) require(failure == RUNTIME);
        else if (mode == 3 || mode == 6 || mode == 9 || mode == 13 || mode == 16) require(failure == ERROR);
        else if (mode == 1 || mode == 4 || mode == 7 || mode == 10 || mode == 14) require(failure instanceof NullPointerException);
        else if (mode == 11 || mode == 17 || mode == 22 || mode == 23) require(failure instanceof ClassCastException);
        else require(failure == null && result == expected);
        require(index.size() == 1 && index.get(key) == value);
        System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
    }
    private static void standard(final int mode, int attempt) {
        final Object wrong = new Object(), key = new Object(), otherKey = new Object();
        final String one = new String("owned one"), two = new String("owned two");
        final String firstValue = new String("owned first value"), secondValue = new String("owned second value");
        final String seed = new String("owned seed"), firstMerged = new String("owned first merge"), secondMerged = new String("owned second merge");
        final Object[] elements = mode == 1 ? new Object[0] : new Object[]{mode == 21 ? wrong : one, two};
        final Map index = new LinkedHashMap();
        if (mode == 20 || mode == 23) index.put(key, mode == 20 ? seed : null);
        final int[] calls = new int[4];
        final boolean[] collecting = new boolean[1];
        Trace.targets = 0; Trace.events = 0;
        Function first = mode == 10 ? null : new Function<String, Object>() {
            public Object apply(String input) {
                require(collecting[0] && input == elements[calls[0]]); calls[0]++; Trace.hit(2);
                if (mode == 3) throw RUNTIME;
                if (mode == 4) throw ERROR;
                return mode == 19 ? null : mode == 2 && calls[0] == 2 ? otherKey : key;
            }
        };
        final Function rawValue = new Function() {
            public Object apply(Object input) {
                require(collecting[0] && input == elements[calls[1]]); calls[1]++; Trace.hit(3);
                if (mode == 5) throw RUNTIME;
                if (mode == 6) throw ERROR;
                return mode == 18 ? null : mode == 22 && calls[1] == 1 ? wrong : calls[1] == 1 ? firstValue : secondValue;
            }
        };
        Function second = mode == 11 ? null : mode == 22 ? rawValue : new Function<String, String>() {
            public String apply(String input) { return (String)rawValue.apply(input); }
        };
        BinaryOperator merger = mode == 12 ? null : new BinaryOperator<String>() {
            public String apply(String left, String right) {
                require(collecting[0]);
                Object actualKey = mode == 19 ? null : key;
                require(left == (mode == 20 ? calls[2] == 0 ? seed : firstMerged : firstValue));
                require(right == (mode == 20 && calls[2] == 0 ? firstValue : secondValue));
                require(index.get(actualKey) == left);
                calls[2]++; Trace.hit(4);
                if (mode == 7) throw RUNTIME;
                if (mode == 8) throw ERROR;
                return mode == 9 ? null : calls[2] == 1 ? firstMerged : secondMerged;
            }
        };
        Supplier supplier = mode == 13 ? null : new Supplier() {
            public Object get() {
                require(collecting[0] && calls[0] == 0 && calls[1] == 0 && calls[2] == 0);
                calls[3]++; Trace.hit(1);
                if (mode == 16) throw RUNTIME;
                if (mode == 17) throw ERROR;
                return mode == 14 ? wrong : mode == 15 ? null : index;
            }
        };
        Object result = null; Throwable failure = null; Collector target = null;
        try { target = (Collector)MergingCollectorCaller.call(3, first, second, merger, supplier); }
        catch (Throwable caught) { failure = caught; }
        require(failure == null && target != null && Trace.targets == 0 && Trace.events == 0);
        require(calls[0] == 0 && calls[1] == 0 && calls[2] == 0 && calls[3] == 0);
        collecting[0] = true;
        try { result = Arrays.asList(elements).stream().collect(target); }
        catch (Throwable caught) { failure = caught; }
        final int[] keyCounts = {2,0,2,1,1,1,1,2,2,2,0,1,1,0,0,1,0,0,1,2,2,0,2,2};
        final int[] valueCounts = {2,0,2,0,0,1,1,2,2,2,0,0,1,0,0,1,0,0,1,2,2,0,2,2};
        final int[] mergeCounts = {1,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,1,2,0,0,1};
        final long[] events = {123234,1,12323,12,12,123,123,123234,123234,123234,1,12,123,0,1,123,1,1,123,123234,1234234,1,12323,123234};
        require(calls[0] == keyCounts[mode] && calls[1] == valueCounts[mode]);
        require(calls[2] == mergeCounts[mode] && calls[3] == (mode == 13 ? 0 : 1));
        require(Trace.targets == 0 && Trace.events == events[mode]);
        if (mode == 3 || mode == 5 || mode == 7 || mode == 16) require(failure == RUNTIME);
        else if (mode == 4 || mode == 6 || mode == 8 || mode == 17) require(failure == ERROR);
        else if (mode == 10 || mode == 11 || mode == 12 || mode == 13 || mode == 15 || mode == 18)
            require(failure instanceof NullPointerException);
        else if (mode == 14 || mode == 21 || mode == 22) require(failure instanceof ClassCastException);
        else require(failure == null && result == index);
        if (mode == 2) require(index.size() == 2 && index.get(key) == firstValue && index.get(otherKey) == secondValue);
        else if (mode == 0 || mode == 19 || mode == 20 || mode == 23)
            require(index.size() == 1 && index.get(mode == 19 ? null : key) == (mode == 20 ? secondMerged : firstMerged));
        else if (mode == 7 || mode == 8 || mode == 22)
            require(index.size() == 1 && index.get(key) == (mode == 22 ? wrong : firstValue));
        else require(index.isEmpty());
        System.out.println("route=3,mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
    }
    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; attempt++) {
            if (route == 3) standard(mode, attempt); else owned(route, mode, attempt);
        }
    }
}
