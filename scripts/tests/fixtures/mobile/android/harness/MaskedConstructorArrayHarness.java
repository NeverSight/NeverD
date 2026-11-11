import fixture.Core;
import fixture.Factory;
import fixture.Trace;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.util.ArrayList;
import java.util.List;

public final class MaskedConstructorArrayHarness {
    private static final class Checked extends Exception {}
    private static Object[] previousArray;
    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError("owned masked constructor array: " + reason);
    }
    private static <E extends Throwable> void throwAny(Throwable failure) throws E { throw (E)failure; }
    private static final class Observer implements Factory {
        final boolean useFactory;
        final int mode, size;
        final Object prior, selected;
        final Throwable failure;
        final List<Integer> events = new ArrayList<Integer>();
        Object[] array;
        Core before;
        Observer(int branch, int mode) {
            useFactory = branch == 1; this.mode = mode;
            size = mode == 1 || mode == 14 ? -1 : mode == 3 ? 0 : 2;
            prior = mode == 12 ? null : new Object();
            Object result = new Object(), alternate = new Object();
            selected = mode == 11 ? null : mode == 13 ? alternate : result;
            failure = mode == 6 || mode == 8 || mode == 10 ? new AssertionError("callback") : new Checked();
            Trace.core = null; Trace.hook = this;
        }
        Object chosen() { return useFactory ? selected : prior; }
        public Object make(Object[] values) {
            events.add(1);
            require(useFactory && mode != 2 && mode != 14 && before == null && Trace.core == null, "factory scope and timing");
            require(values != null && values != previousArray && values.length == size, "fresh default array and length");
            for (Object element : values) require(element == null, "new default array contents");
            array = values; previousArray = values;
            if (mode == 4) values[0] = prior;
            if (mode == 5 || mode == 6) MaskedConstructorArrayHarness.<RuntimeException>throwAny(failure);
            return selected;
        }
        public void stage(int phase, Object value) {
            events.add(phase);
            require(value instanceof Core, "original constructor receiver");
            if (phase == 2) {
                require(before == null && Trace.core == null && ((Core)value).value == null,
                        "callback before final store");
                require(!useFactory || array != null, "array factory before delegation");
                before = (Core)value;
                if (mode == 7 || mode == 8) MaskedConstructorArrayHarness.<RuntimeException>throwAny(failure);
            } else {
                require(phase == 3 && value == before && value == Trace.core && before.value == chosen(),
                        "final value and escaped receiver identity");
                if (mode == 9 || mode == 10) MaskedConstructorArrayHarness.<RuntimeException>throwAny(failure);
            }
        }
        void verify(Core answer, Throwable caught) {
            boolean negative = useFactory && size < 0;
            boolean nullFactory = useFactory && (mode == 2 || mode == 14);
            boolean factoryFailed = useFactory && (mode == 5 || mode == 6);
            boolean prefixFailed = negative || nullFactory || factoryFailed;
            List<Integer> expected = new ArrayList<Integer>();
            if (useFactory && !negative && !nullFactory) expected.add(1);
            if (!prefixFailed) { expected.add(2); if (mode != 7 && mode != 8) expected.add(3); }
            require(events.equals(expected), "ordered default and constructor events " + events + " expected " + expected);
            boolean failed = prefixFailed || mode >= 7 && mode <= 10;
            if (negative) require(caught != null && caught.getClass() == NegativeArraySizeException.class, "array failure before null factory invocation");
            else if (nullFactory) require(caught != null && caught.getClass() == NullPointerException.class, "null factory invocation");
            else require(caught == (failed ? failure : null), "checked and Error identity");
            require((answer == null) == failed && (answer == null || answer == before), "constructed result identity");
            require((before != null) == !prefixFailed, "delegated initialization boundary");
            if (before != null) {
                require(before.value == (mode == 7 || mode == 8 ? null : chosen()), "partial final assignment");
                require(Trace.core == (mode == 7 || mode == 8 ? null : before), "partial receiver escape");
            } else require(Trace.core == null, "no receiver escape before delegation");
            if (array != null && array.length > 0) require(array[0] == (mode == 4 ? prior : null), "retained factory array mutation");
        }
    }
    private static Core compiledCaller(Object prior, Factory factory, int size, int mask) {
        return new Core(prior, factory, size, mask);
    }
    private static Core call(int route, Object prior, Factory factory, int size, int mask) throws Throwable {
        if (route == 0) {
            try { return Core.class.getConstructor(Object.class, Factory.class, int.class, int.class).newInstance(prior, factory, size, mask); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (route == 1) {
            MethodHandle handle = MethodHandles.publicLookup().findConstructor(Core.class,
                    MethodType.methodType(void.class, Object.class, Factory.class, int.class, int.class));
            return (Core)handle.invokeExact(prior, factory, size, mask);
        }
        return compiledCaller(prior, factory, size, mask);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), branch = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        for (int attempt = 0; attempt < 2; ++attempt) for (int repeat = 0; repeat < (mode == 15 ? 2 : 1); ++repeat) {
            Observer observer = new Observer(branch, mode); Core answer = null; Throwable failure = null;
            Factory factory = mode == 2 || mode == 14 ? null : observer;
            try { answer = call(route, observer.prior, factory, observer.size, branch == 1 ? 17 : 16); }
            catch (Throwable caught) { failure = caught; }
            observer.verify(answer, failure);
        }
        System.out.println("attempts=2,masked-constructor-array=matched");
    }
}
