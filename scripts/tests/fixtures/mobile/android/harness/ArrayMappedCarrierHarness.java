import fixture.Core;
import fixture.Trace;
import java.util.AbstractSet;
import java.util.HashSet;
import java.util.Iterator;
import java.util.function.Function;

public final class ArrayMappedCarrierHarness {
    private static void check(boolean value, String detail) {
        if (!value) throw new AssertionError("owned array mapped carrier mismatch: " + detail);
    }
    private static final class OpaqueSet extends HashSet<Object> {
        public Iterator<Object> iterator() { throw new AssertionError("owned result observed"); }
        public int size() { throw new AssertionError("owned result observed"); }
        public boolean add(Object value) { throw new AssertionError("owned result observed"); }
        public boolean contains(Object value) { throw new AssertionError("owned result observed"); }
    }
    private static final class OtherSet extends AbstractSet<Object> {
        public Iterator<Object> iterator() { throw new AssertionError("owned result observed"); }
        public int size() { throw new AssertionError("owned result observed"); }
    }
    private static Object call(int route, Object[] source, Function<Object,Object> mapper) {
        switch (route) {
            case 0: return Core.raw(source,mapper);
            case 1: return Core.narrow(source,mapper);
            case 2: return Core.ignored(source,mapper);
            case 3: return Core.relay(source,mapper);
            case 4: return Core.typed(source,mapper);
            case 5: return Core.alias(source,mapper);
            default: throw new AssertionError("owned route");
        }
    }
    private static void attempt(final int route, final int mode, int attempt) {
        final Object item = mode == 18 || mode >= 20 ? new String("owned input") : new Object();
        final Object other = mode == 18 || mode >= 20 ? new String("owned other") : new Object();
        final Object saved = mode == 20 ? new String("owned store") : mode == 21 ? null : new Object();
        final Object poison = new Object(), mapped = new Object(), marker = new Object();
        final Object[] source = mode == 4 ? null : mode == 5 ? new Object[0]
                : mode == 18 || mode >= 20 ? new String[]{(String)item,(String)other} : new Object[]{item,other};
        final Object[] replacementArray = new Object[]{poison,poison};
        final OpaqueSet result = new OpaqueSet();
        final OtherSet late = new OtherSet();
        final RuntimeException runtime = new IllegalStateException("owned runtime");
        final Error error = new LinkageError("owned error");
        final int[] maps = {0}, actions = {0}, stale = {0};
        final Function[] selectedMapper = {null};
        Trace.calls = Trace.relays = Trace.stage = 0;
        Trace.index = mode == 6 ? -1 : mode == 7 ? 2 : 0;
        Trace.source = null;
        Trace.mapper = Trace.item = Trace.mapped = null;
        Trace.value = mode == 1 ? source : mode == 2 ? null : mode == 3 ? late : result;
        Trace.store = saved;
        Trace.marker = marker;
        final Runnable action = new Runnable() {
            public void run() {
                actions[0]++;
                check(Trace.calls == 1 && Trace.stage == 3 && maps[0] == 1, "action order");
                check(Trace.source == source && Trace.mapper == selectedMapper[0]
                        && Trace.item == item && Trace.mapped == (mode == 11 ? null : mapped), "action snapshot");
                if (mode == 14) throw runtime;
                if (mode == 15) throw error;
                if (mode == 12) { Trace.value = late; Trace.marker = poison; }
                if (mode == 17) { Trace.source = replacementArray; Trace.mapper = poison; Trace.index = 1; }
                if (mode == 19) Trace.store = poison;
            }
        };
        final Function<Object,Object> mapper = mode == 8 ? null : new Function<Object,Object>() {
            public Object apply(Object received) {
                maps[0]++;
                check(Trace.stage == 2 && Trace.calls == 1 && received == item
                        && Trace.item == item && Trace.source == source
                        && Trace.mapper == selectedMapper[0], "mapper snapshot");
                if (mode == 9) throw runtime;
                if (mode == 10) throw error;
                if (mode == 16) Trace.action = action;
                return mode == 11 ? null : mapped;
            }
        };
        selectedMapper[0] = mapper;
        Trace.action = mode == 13 ? null : mode == 16 ? new Runnable() {
            public void run() { stale[0]++; }
        } : action;
        Object value = null;Throwable failure = null;
        try { value = call(route,source,mapper); } catch (Throwable caught) { failure = caught; }
        int stage = 4;Class<?> kind = null;Throwable exact = null;
        if (mode == 4) { stage = 1; kind = NullPointerException.class; }
        else if (mode == 5 || mode == 6 || mode == 7) { stage = 1; kind = ArrayIndexOutOfBoundsException.class; }
        else if (mode == 8) { stage = 2; kind = NullPointerException.class; }
        else if (mode == 9 || mode == 10) { stage = 2; exact = mode == 9 ? runtime : error; }
        else if (mode == 13) { stage = 3; kind = NullPointerException.class; }
        else if (mode == 14 || mode == 15) { stage = 3; exact = mode == 14 ? runtime : error; }
        else if (mode == 18) { stage = 3; kind = ArrayStoreException.class; }
        else if (mode == 1 || route == 1 && (mode == 3 || mode == 12)) kind = ClassCastException.class;
        if (exact != null) check(failure == exact,"failure identity");
        else if (kind != null) check(failure != null && failure.getClass() == kind,"failure class");
        else check(failure == null && value == (route == 2 ? mode == 12 ? poison : marker
                : mode == 2 ? null : mode == 3 || mode == 12 ? late : result),"actual returned reference");
        check(Trace.calls == 1 && Trace.relays == (route == 3 ? 1 : 0) && Trace.stage == stage,"entry and stage");
        check(maps[0] == (stage == 1 || mode == 8 ? 0 : 1)
                && actions[0] == (stage < 3 || mode == 13 ? 0 : 1) && stale[0] == 0,"effect counts");
        check(Trace.source == (mode == 17 ? replacementArray : source)
                && Trace.mapper == (mode == 17 ? poison : mapper),"original argument references");
        check(Trace.item == (stage == 1 ? null : item)
                && Trace.mapped == (stage <= 2 || mode == 11 ? null : mapped),"read snapshots");
        check(Trace.store == (mode == 19 ? poison : saved),"store global");
        if (source != null && source.length == 2) {
            check(source[0] == (stage == 4 && mode != 17 ? saved : item)
                    && source[1] == (mode == 17 ? saved : other),"original array store and index");
        }
        check(replacementArray[0] == poison && replacementArray[1] == poison,"replacement array untouched");
    }
    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; attempt++) attempt(route,mode,attempt);
        System.out.println("attempts=2,array-mapped-carrier=matched");
    }
}
