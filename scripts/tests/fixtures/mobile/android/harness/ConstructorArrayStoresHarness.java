import fixture.Core;
import fixture.Factory;
import fixture.Trace;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.util.ArrayList;
import java.util.List;

public final class ConstructorArrayStoresHarness {
    private static final class Checked extends Exception {}
    private static Object[] previousArray;
    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError("owned constructor array stores: " + reason);
    }
    private static <E extends Throwable> void throwAny(Throwable failure) throws E { throw (E)failure; }
    private static final class Observer implements Factory {
        final int mode, size, index;
        final Object old = new Object(), seed, result;
        final Object[] input;
        final Throwable failure;
        final List<Integer> events = new ArrayList<Integer>();
        Object[] seen;
        Core before;
        Observer(int mode) {
            this.mode = mode; size = mode == 1 ? -1 : mode == 3 ? 0 : 2;
            index = mode == 2 ? -1 : mode == 15 ? size : 0;
            seed = mode == 4 || mode == 16 ? new Object() : mode == 13 ? null : new String("seed");
            result = mode == 12 ? null : new Object();
            input = mode == 14 ? null : new Object[] {old};
            failure = mode == 7 || mode == 9 || mode == 11 ? new AssertionError("callback") : new Checked();
            Trace.core = null; Trace.hook = this;
        }
        public Object make(Object[] values, Object value) {
            events.add(1);
            require(value == seed && values != input && values != previousArray && values instanceof String[]
                    && values.length == size && values[index] == seed, "original store, assignment result and fresh array");
            for (int i = 0; i < values.length; ++i) require(values[i] == (i == index ? seed : null), "array store contents");
            seen = values; previousArray = values;
            require(before == null && Trace.core == null, "factory precedes delegated receiver");
            if (mode == 6 || mode == 7) ConstructorArrayStoresHarness.<RuntimeException>throwAny(failure);
            return result;
        }
        public void stage(int phase, Object value) {
            events.add(phase);
            require(value instanceof Core && seen != null, "factory before delegated constructor");
            if (phase == 2) {
                require(before == null && Trace.core == null && ((Core)value).value == null, "callback before final store");
                before = (Core)value;
                if (mode == 8 || mode == 9) ConstructorArrayStoresHarness.<RuntimeException>throwAny(failure);
            } else {
                require(phase == 3 && value == before && value == Trace.core && before.value == result,
                        "final store and escaped receiver identity");
                if (mode == 10 || mode == 11) ConstructorArrayStoresHarness.<RuntimeException>throwAny(failure);
            }
        }
        void verify(Core answer, Throwable caught) {
            boolean negative = size < 0;
            boolean bounds = !negative && (index < 0 || index >= size);
            boolean covariance = !negative && !bounds && seed != null && !(seed instanceof String);
            boolean nullFactory = mode == 5 || mode == 15 || mode == 16;
            boolean prefixFailed = negative || bounds || covariance || nullFactory;
            boolean factoryFailed = mode == 6 || mode == 7;
            List<Integer> expected = new ArrayList<Integer>();
            if (!prefixFailed) {
                expected.add(1);
                if (!factoryFailed) { expected.add(2); if (mode != 8 && mode != 9) expected.add(3); }
            }
            require(events.equals(expected), "ordered array and constructor events " + events + " expected " + expected);
            if (negative) require(caught != null && caught.getClass() == NegativeArraySizeException.class, "negative size before invocation");
            else if (bounds) require(caught != null && caught.getClass() == ArrayIndexOutOfBoundsException.class, "bounds before null factory invocation");
            else if (covariance) require(caught != null && caught.getClass() == ArrayStoreException.class, "array store before null factory invocation");
            else if (nullFactory) require(caught != null && caught.getClass() == NullPointerException.class, "null factory invocation");
            else require(caught == (mode >= 6 && mode <= 11 ? failure : null), "original checked and Error identity");
            boolean failed = prefixFailed || mode >= 6 && mode <= 11;
            require((answer == null) == failed && (answer == null || answer == before), "constructor result identity");
            require((before != null) == (!prefixFailed && !factoryFailed), "delegation boundary");
            if (before != null) {
                require(before.value == (mode == 8 || mode == 9 ? null : result), "partial final assignment");
                require(Trace.core == (mode == 8 || mode == 9 ? null : before), "partial receiver escape");
            } else require(Trace.core == null, "no early receiver escape");
            if (input != null) require(input.length == 1 && input[0] == old, "overwritten parameter preserves original array");
        }
    }
    private static Core compiledCaller(Object[] buffer, int size, int index, Object seed, Factory factory) {
        return new Core(buffer, size, index, seed, factory);
    }
    private static Core call(int route, Object[] buffer, int size, int index, Object seed, Factory factory) throws Throwable {
        if (route == 0) {
            try { return Core.class.getConstructor(Object[].class, int.class, int.class, Object.class, Factory.class)
                    .newInstance(buffer, size, index, seed, factory); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (route == 1) {
            MethodHandle handle = MethodHandles.publicLookup().findConstructor(Core.class,
                    MethodType.methodType(void.class, Object[].class, int.class, int.class, Object.class, Factory.class));
            return (Core)handle.invokeExact(buffer, size, index, seed, factory);
        }
        return compiledCaller(buffer, size, index, seed, factory);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; ++attempt) for (int repeat = 0; repeat < (mode == 17 ? 2 : 1); ++repeat) {
            Observer observer = new Observer(mode); Core answer = null; Throwable failure = null;
            Factory factory = mode == 5 || mode == 15 || mode == 16 ? null : observer;
            try { answer = call(route, observer.input, observer.size, observer.index, observer.seed, factory); }
            catch (Throwable caught) { failure = caught; }
            observer.verify(answer, failure);
        }
        System.out.println("attempts=2,constructor-array-stores=matched");
    }
}
