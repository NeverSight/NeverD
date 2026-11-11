import fixture.Trace;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.List;
import java.util.function.Supplier;
import java.util.stream.Collector;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class ProducedCollectorHarness {
    private static final RuntimeException RUNTIME = new RuntimeException("owned supplier failure");
    private static final Error ERROR = new AssertionError("owned supplier error");
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned produced collector mismatch");
    }
    private static Collector collector() {
        return Collector.of(ArrayList::new, ArrayList::add,
                (ArrayList a, ArrayList b) -> { a.addAll(b); return a; });
    }
    private static void owned(int route, final int mode, int attempt) {
        final Collector original = collector(), other = collector();
        final Object wrong = new Object(), item = new Object();
        final Collection items = new ArrayList(), otherItems = new ArrayList();
        items.add(item);
        final int[] calls = new int[3];
        Trace.targets = 0; Trace.events = 0; Trace.supplier = null; Trace.collection = null;
        Trace.result = mode == 9 ? wrong : mode == 10 ? null : original;
        final Runnable alternate = new Runnable() {
            public void run() { require(Trace.events == 12); calls[2]++; }
        };
        Trace.action = mode == 8 ? null : new Runnable() {
            public void run() {
                require(Trace.events == 12); calls[1]++;
                if (mode == 6) throw RUNTIME;
                if (mode == 7) throw ERROR;
                if (mode == 11) Trace.result = other;
            }
        };
        Supplier supplier = mode == 1 ? null : new Supplier() {
            public Object get() {
                require(Trace.events == 1 && Trace.targets == 1 && Trace.collection == null);
                calls[0]++;
                if (mode == 2) throw RUNTIME;
                if (mode == 3) throw ERROR;
                if (mode == 12) Trace.result = other;
                if (mode == 13) Trace.collection = otherItems;
                if (mode == 14) Trace.action = alternate;
                if (mode == 15) Trace.result = wrong;
                return mode == 4 ? wrong : mode == 5 ? null : items;
            }
        };
        Object result = null; Throwable failure = null;
        try { result = ProducedCollectorCaller.call(route, supplier); }
        catch (Throwable caught) { failure = caught; }
        int stage = mode >= 1 && mode <= 4 ? 1 : mode >= 6 && mode <= 8 ? 2 : 3;
        require(Trace.targets == 1 && Trace.events == (stage == 1 ? 1 : stage == 2 ? 12 : 123));
        require(Trace.supplier == supplier && calls[0] == (mode == 1 ? 0 : 1));
        require(calls[1] == (stage >= 2 && mode != 8 && mode != 14 ? 1 : 0));
        require(calls[2] == (mode == 14 ? 1 : 0));
        require(Trace.collection == (stage >= 2 && mode != 5 ? items : null));
        if (mode == 2 || mode == 6) require(failure == RUNTIME);
        else if (mode == 3 || mode == 7) require(failure == ERROR);
        else if (mode == 1 || mode == 8) require(failure instanceof NullPointerException);
        else if (mode == 4 || mode == 9 || mode == 15) require(failure instanceof ClassCastException);
        else { require(failure == null); require(result == (mode == 11 || mode == 12 ? other : mode == 10 ? null : original)); }
        require(items.size() == 1 && items.iterator().next() == item && otherItems.isEmpty());
        require(Trace.result == (mode == 9 || mode == 15 ? wrong : mode == 10 ? null : mode == 11 || mode == 12 ? other : original));
        System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
    }
    private static void standard(final int mode, int attempt) {
        final Object one = new Object(), two = new Object(), seed = new Object(), wrong = new Object();
        final int[] calls = new int[2];
        final List items = new ArrayList() {
            public boolean add(Object value) {
                calls[1]++;
                if (mode == 6 || mode == 14 && calls[1] == 2) throw RUNTIME;
                if (mode == 7) throw ERROR;
                return super.add(value);
            }
        };
        if (mode == 9) { items.add(seed); calls[1] = 0; }
        Supplier supplier = mode == 15 ? null : new Supplier() {
            public Object get() {
                calls[0]++;
                if (mode == 2) throw RUNTIME;
                if (mode == 3) throw ERROR;
                return mode == 4 ? null : mode == 5 ? wrong : items;
            }
        };
        List inputs = mode == 1 ? Collections.emptyList() : mode == 13 ? Collections.singletonList(one)
                : Arrays.asList(mode == 8 ? null : one, mode == 10 ? one : two);
        Trace.targets = 0;
        Collector collector = null; Object result = null; Throwable failure = null; int stage = 1;
        try { collector = ProducedCollectorCaller.standard(supplier); }
        catch (Throwable caught) { failure = caught; }
        require(Trace.targets == 0 && calls[0] == 0 && calls[1] == 0);
        if (failure == null) {
            require(collector != null); stage = 2;
            try { result = inputs.stream().collect(collector); }
            catch (Throwable caught) { failure = caught; }
        } else require(mode == 15 && failure instanceof NullPointerException);
        if (mode == 2 || mode == 6 || mode == 14) require(failure == RUNTIME);
        else if (mode == 3 || mode == 7) require(failure == ERROR);
        else if (mode == 4 || mode == 15) require(failure instanceof NullPointerException);
        else if (mode == 5) require(failure instanceof ClassCastException);
        else { require(failure == null); require(result == items); }
        int additions = mode >= 1 && mode <= 5 || mode == 15 ? 0
                : mode == 6 || mode == 7 || mode == 13 ? 1 : 2;
        require(calls[0] == (mode == 15 ? 0 : 1) && calls[1] == additions);
        List expected = new ArrayList();
        if (mode == 9) expected.add(seed);
        if (mode == 14) expected.add(one);
        else if (failure == null) expected.addAll(inputs);
        require(items.size() == expected.size());
        for (int i = 0; i < expected.size(); i++) require(items.get(i) == expected.get(i));
        System.out.println("route=3,mode=" + mode + ",attempt=" + attempt + ",trace=" + (stage * 100 + calls[0] * 10 + calls[1]));
    }
    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; attempt++) {
            if (route == 3) standard(mode, attempt); else owned(route, mode, attempt);
        }
    }
}
