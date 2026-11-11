import fixture.Core;
import fixture.Parcel;
import fixture.Probe;
import fixture.Trace;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.util.ArrayList;
import java.util.List;

public final class ConstructorAllocationArraysHarness {
    private static final class Checked extends Exception {}
    private static boolean initialized, poisoned;
    private static Object lastArray;
    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError("owned constructor allocation arrays: " + reason);
    }
    private static <E extends Throwable> void throwAny(Throwable failure) throws E { throw (E)failure; }
    private static final class Observer implements Probe {
        final int mode, size;
        final Object seed = new Object(), sentinel = new Object();
        final boolean initAtEntry = initialized, poisonAtEntry = poisoned;
        final Throwable failure;
        final List<Integer> events = new ArrayList<Integer>();
        int lastPhase;
        Observer(int mode) {
            this.mode = mode; size = mode == 1 ? -1 : mode == 2 ? 0 : 2;
            failure = mode == 4 || mode == 6 || mode == 8 || mode == 10
                    ? new AssertionError("callback") : new Checked();
            Trace.array = sentinel; Trace.parcel = sentinel; Trace.core = sentinel;
            Trace.after = false; Trace.probe = this;
        }
        public void hit(int phase, Object value) {
            events.add(phase);
            require(phase == (lastPhase == 0 ? (initialized ? 2 : 1) : lastPhase + 1), "effect order");
            lastPhase = phase;
            require(!Trace.after, "post-construction effect moved early");
            if (phase == 1) {
                require(value == null && Trace.array == sentinel && Trace.parcel == sentinel && Trace.core == sentinel,
                        "allocation initialization precedes array creation");
                if (mode == 9 || mode == 10) ConstructorAllocationArraysHarness.<RuntimeException>throwAny(failure);
                initialized = true;
            } else if (phase == 2) {
                require(value == Trace.array && value instanceof Object[] && ((Object[])value).length == size,
                        "fresh array length and identity");
                require(value != lastArray && Trace.parcel == sentinel && Trace.core == sentinel, "fresh array or early constructor effect");
                lastArray = value;
                for (Object element : (Object[])value) require(element == null, "new array contents");
            } else if (phase == 3) {
                require(value == Trace.parcel && value instanceof Parcel && ((Parcel)value).values == Trace.array
                        && Trace.core == sentinel, "argument initialization before delegation");
            } else if (phase == 4) {
                require(value == Trace.core && value instanceof Core && ((Core)value).seed == seed
                        && ((Core)value).parcel == Trace.parcel, "original field assignments before escape");
            } else require(false, "unexpected callback");
            if ((mode == 11 && phase == 2) || (mode == 12 && phase == 3) || (mode == 13 && phase == 4))
                ((Object[])Trace.array)[0] = seed;
            if ((phase == 2 && (mode == 3 || mode == 4)) || (phase == 3 && (mode == 5 || mode == 6))
                    || (phase == 4 && (mode == 7 || mode == 8)))
                ConstructorAllocationArraysHarness.<RuntimeException>throwAny(failure);
        }
        void verify(Core answer, Throwable caught) {
            int stop = poisonAtEntry ? 0 : !initAtEntry && (mode == 9 || mode == 10) ? 1
                    : size < 0 ? (initAtEntry ? 0 : 1) : mode == 3 || mode == 4 ? 2 : mode == 5 || mode == 6 ? 3 : 4;
            List<Integer> expected = new ArrayList<Integer>();
            if (!poisonAtEntry) for (int phase = initAtEntry ? 2 : 1; phase <= stop; ++phase) expected.add(phase);
            require(events.equals(expected), "callback inventory " + events + " expected " + expected);
            boolean failed = poisonAtEntry || size < 0 || stop < 4 || mode == 7 || mode == 8;
            if (poisonAtEntry) require(caught != null && caught.getClass() == NoClassDefFoundError.class, "poisoned allocated class");
            else if (!initAtEntry && (mode == 9 || mode == 10)) {
                if (mode == 9) require(caught instanceof ExceptionInInitializerError && caught.getCause() == failure, "checked initializer failure");
                else require(caught == failure, "initializer Error identity");
                poisoned = true;
            } else if (size < 0) require(caught != null && caught.getClass() == NegativeArraySizeException.class, "negative array size");
            else if (mode >= 3 && mode <= 8) require(caught == failure, "callback exception identity");
            else require(caught == null, "unexpected construction failure");
            require((answer != null) == !failed && (answer == null || answer == Trace.core), "returned constructor identity");
            require(Trace.after == !failed, "caller completion effect");
            require((Trace.array != sentinel) == (stop >= 2), "array escape boundary");
            require((Trace.parcel != sentinel) == (stop >= 3), "argument constructor boundary");
            require((Trace.core != sentinel) == (stop == 4), "delegating constructor boundary");
            if (stop >= 3) require(((Parcel)Trace.parcel).values == Trace.array, "argument array alias");
            if (stop >= 2 && size > 0) require(((Object[])Trace.array)[0] == (mode >= 11 && mode <= 13 ? seed : null), "retained array mutation");
        }
    }
    private static Core compiledCaller(Object seed, int size) { return new Core(seed, size); }
    private static Core call(int route, Object seed, int size) throws Throwable {
        if (route == 0) {
            try { return Core.class.getConstructor(Object.class, int.class).newInstance(seed, size); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (route == 1) {
            MethodHandle handle = MethodHandles.publicLookup().findConstructor(Core.class,
                    MethodType.methodType(void.class, Object.class, int.class));
            return (Core)handle.invokeExact(seed, size);
        }
        return compiledCaller(seed, size);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; ++attempt) for (int repeat = 0; repeat < (mode == 14 ? 2 : 1); ++repeat) {
            Observer observer = new Observer(mode); Core answer = null; Throwable failure = null;
            try { answer = call(route, observer.seed, observer.size); } catch (Throwable caught) { failure = caught; }
            observer.verify(answer, failure);
        }
        System.out.println("attempts=2,constructor-allocation-arrays=matched");
    }
}
