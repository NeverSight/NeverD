import fixture.Client;
import fixture.Probe;
import fixture.Receipt;
import fixture.Trace;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.util.ArrayList;
import java.util.List;

public final class ArrayNominalResultHarness {
    private static final class Checked extends Exception {}
    private static boolean initialized;
    private static boolean poisoned;
    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError("owned nominal array result: " + reason);
    }
    private static <E extends Throwable> void throwAny(Throwable failure) throws E {
        throw (E)failure;
    }
    private static final class Observer implements Probe {
        final int kind, mode;
        final Object[] values;
        final Object oldValue, replacement, laterValue, sentinel = new Object();
        final Receipt selected, alternate;
        final Throwable failure;
        final boolean write;
        final List<Integer> events = new ArrayList<Integer>();
        final boolean initAtEntry, poisonAtEntry;
        int lastPhase;

        Observer(int kind, int mode, int attempt) {
            this.kind = kind; this.mode = mode;
            initAtEntry = initialized; poisonAtEntry = poisoned;
            oldValue = kind == 1 ? new String("old") : new Object();
            replacement = mode == 5 ? new Object() : kind == 1 ? new String("replacement") : new Object();
            laterValue = kind == 1 ? new String("later") : new Object();
            values = mode == 1 ? null : kind == 1
                    ? (mode == 2 || mode == 3 ? new String[0] : new String[] {(String)oldValue})
                    : (mode == 2 || mode == 3 ? new Object[0] : new Object[] {oldValue});
            selected = mode == 12 ? null : new Receipt(new Object(), attempt + 71);
            alternate = new Receipt(new Object(), attempt + 193);
            failure = mode == 7 || mode == 9 || mode == 11 ? new AssertionError("callback") : new Checked();
            write = mode == 3 || mode == 4 || mode == 5 || mode == 16;
            Trace.seen = sentinel; Trace.after = sentinel; Trace.length = -1;
            Trace.result = selected; Trace.alternate = alternate;
            Trace.replacement = replacement; Trace.write = write;
            Trace.probe = this;
        }
        Receipt chosen() { return mode == 14 ? alternate : selected; }
        void throwFailure() { ArrayNominalResultHarness.<RuntimeException>throwAny(failure); }
        public void hit(int phase, Object value) {
            events.add(phase);
            require(phase != 8 && phase != 9, "wrong overload or facade initialization");
            require(phase == (lastPhase == 0 ? (initialized ? 2 : 1) : lastPhase + 1), "callback order");
            lastPhase = phase;
            require(Trace.after == sentinel, "caller effect before provider return");
            if (phase == 1) {
                require(value == null && Trace.seen == sentinel && Trace.length == -1, "initialization effects");
                if (mode == 10 || mode == 11) throwFailure();
                initialized = true;
            } else if (phase == 2) {
                require(value == values && Trace.seen == values && Trace.length == -1, "original array identity and early effects");
                if (mode == 6 || mode == 7) throwFailure();
            } else if (phase == 3) {
                require(value == values && Trace.seen == values && Trace.length == values.length, "array length or late identity");
                if (values.length != 0) require(values[0] == (write ? replacement : oldValue), "provider array store");
                if (mode == 8) throwFailure();
                if (mode == 14) Trace.result = alternate;
                if (mode == 16) values[0] = laterValue;
            } else if (phase == 4) {
                require(value == chosen(), "result read order or identity");
                if (mode == 9) throwFailure();
                if (mode == 15) Trace.result = alternate;
            }
        }
        int stopPhase() {
            if (poisonAtEntry) return 0;
            if (!initAtEntry && (mode == 10 || mode == 11)) return 1;
            if (mode == 1 || mode == 3 || mode == 6 || mode == 7 || (mode == 5 && kind == 1)) return 2;
            if (mode == 8) return 3;
            return 4;
        }
        void verify(Object answer, Throwable caught) {
            int stop = stopPhase();
            List<Integer> expected = new ArrayList<Integer>();
            if (!poisonAtEntry) for (int phase = initAtEntry ? 2 : 1; phase <= stop; ++phase) expected.add(phase);
            require(events.equals(expected), "event inventory " + events + " expected " + expected);
            boolean failed = poisonAtEntry || stop < 4 || mode == 9;
            if (poisonAtEntry) require(caught != null && caught.getClass() == NoClassDefFoundError.class, "poisoned declaring class");
            else if (stop == 1) {
                if (mode == 10) require(caught instanceof ExceptionInInitializerError && caught.getCause() == failure, "checked class initializer exception");
                else require(caught == failure, "class initializer Error identity");
                poisoned = true;
            } else if (mode == 6 || mode == 7 || mode == 8 || mode == 9) require(caught == failure, "callback failure identity");
            else if (mode == 1) require(caught != null && caught.getClass() == NullPointerException.class, "null array failure");
            else if (mode == 3) require(caught != null && caught.getClass() == ArrayIndexOutOfBoundsException.class, "empty array store");
            else if (mode == 5 && kind == 1) require(caught != null && caught.getClass() == ArrayStoreException.class, "covariant array store");
            else require(caught == null, "unexpected provider failure");
            require(answer == (failed ? null : chosen()), "returned nominal identity");
            require(Trace.after == (failed ? sentinel : chosen()), "caller post-return effect");
            require(Trace.seen == (stop >= 2 ? values : sentinel), "escaped array effects");
            require(Trace.length == (stop >= 3 || (stop == 2 && mode != 1 && mode != 6 && mode != 7) ? values.length : -1), "length effect boundary");
            if (values != null && values.length != 0) {
                Object expectedElement = stop >= 3 && write ? replacement : oldValue;
                if (mode == 16 && stop >= 3) expectedElement = laterValue;
                require(values[0] == expectedElement, "final array mutation");
            }
        }
    }
    private static Object compiledCaller(int kind, Object[] values) {
        return kind == 1 ? Client.strings((String[])values) : Client.objects(values);
    }
    private static Object call(int route, int kind, Object[] values) throws Throwable {
        String name = kind == 1 ? "strings" : "objects";
        Class<?> type = kind == 1 ? String[].class : Object[].class;
        if (route == 0) {
            try { return Client.class.getMethod(name, type).invoke(null, new Object[] {values}); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        if (route == 1) {
            MethodHandle handle = MethodHandles.publicLookup().findStatic(Client.class, name,
                    MethodType.methodType(Receipt.class, type)).asType(MethodType.methodType(Object.class, Object[].class));
            return (Object)handle.invokeExact(values);
        }
        return compiledCaller(kind, values);
    }
    private static boolean predicate(int route, Receipt value, Comparable argument) throws Throwable {
        Class<?> api = Class.forName("fixture.Api");
        if (route == 0) {
            try { return ((Boolean)api.getMethod("contains", Comparable.class).invoke(value, new Object[] {argument})).booleanValue(); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (route == 1) {
            MethodHandle handle = MethodHandles.publicLookup().findVirtual(api, "contains",
                    MethodType.methodType(boolean.class, Comparable.class))
                    .asType(MethodType.methodType(boolean.class, Object.class, Comparable.class));
            return (boolean)handle.invokeExact((Object)value, argument);
        }
        try { return ((Boolean)Class.forName("ArrayNominalPredicateCaller").getMethod("call", Receipt.class, Comparable.class)
                .invoke(null, value, argument)).booleanValue(); }
        catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    private static void verifyPredicate(int route, Receipt value) throws Throwable {
        require(predicate(route, value, Character.valueOf((char)value.stamp)), "nominal predicate value");
        require(!predicate(route, value, Character.valueOf((char)(value.stamp + 1))), "nominal predicate comparison");
        for (Comparable argument : new Comparable[] {null, Integer.valueOf(37)}) {
            Throwable caught = null;
            try { predicate(route, value, argument); } catch (Throwable failure) { caught = failure; }
            require(caught != null && caught.getClass() == (argument == null ? NullPointerException.class : ClassCastException.class),
                    "nominal predicate checked conversion");
        }
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), kind = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        for (int attempt = 0; attempt < 2; ++attempt) {
            for (int repeat = 0; repeat < (mode == 17 ? 2 : 1); ++repeat) {
                Observer observer = new Observer(kind, mode, attempt + repeat);
                Object answer = null; Throwable failure = null;
                try { answer = call(route, kind, observer.values); } catch (Throwable caught) { failure = caught; }
                observer.verify(answer, failure);
                if (args.length > 3 && Boolean.parseBoolean(args[3]) && answer != null) verifyPredicate(route, (Receipt)answer);
            }
        }
        System.out.println("attempts=2,array-nominal=matched");
    }
}
