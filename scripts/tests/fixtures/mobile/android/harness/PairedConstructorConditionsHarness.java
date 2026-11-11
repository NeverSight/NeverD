import fixture.Core;
import fixture.Factory;
import fixture.Trace;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.util.ArrayList;
import java.util.List;

public final class PairedConstructorConditionsHarness {
    private static final class Checked extends Exception {}
    private static final class Identity {
        public boolean equals(Object other) { throw new AssertionError("owned paired constructor conditions: equals invoked"); }
    }
    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError("owned paired constructor conditions: " + reason);
    }
    private static <E extends Throwable> void throwAny(Throwable failure) throws E { throw (E)failure; }
    private static final class Observer implements Factory {
        final int mode, start, bound;
        final Object left, right;
        final Object[] results = {new Object(), new Object(), new Object()};
        final Throwable failure;
        final List<Integer> events = new ArrayList<Integer>();
        Core before;
        int steps, stages;
        Observer(int mode) {
            this.mode = mode;
            start = mode == 1 ? 0 : mode == 2 ? 3 : mode == 3 || mode == 5 ? Integer.MIN_VALUE
                    : mode == 4 ? Integer.MAX_VALUE - 2 : -1;
            bound = mode == 1 ? 0 : mode == 2 ? 1 : mode == 3 ? Integer.MIN_VALUE + 2
                    : mode == 4 || mode == 5 ? Integer.MAX_VALUE : 2;
            left = mode == 6 || mode == 9 || mode == 10 ? null : mode == 23 ? new Identity() : new String("equal");
            right = mode == 7 || mode == 9 || mode == 23 ? left : mode == 11 ? null : new String("equal");
            failure = mode == 14 || mode == 16 || mode == 18 || mode == 20 ? new AssertionError("callback") : new Checked();
            Trace.core = null; Trace.hook = this;
        }
        long iterations() { return Math.max(0L, (long)bound - (long)start); }
        Object result() { return iterations() == 0 ? left : results[(int)Math.min(3L, iterations()) - 1]; }
        public Object step(Object value, int index) {
            events.add(1);
            require(steps < 3 && index == start + steps && value == (steps == 0 ? left : results[steps - 1]),
                    "loop index, signed comparison and carried result");
            require(before == null && Trace.core == null, "loop precedes delegated receiver");
            ++steps;
            if (mode == 13 || mode == 14 || mode == 5 && steps == 3)
                PairedConstructorConditionsHarness.<RuntimeException>throwAny(failure);
            return results[steps - 1];
        }
        public void stage(int phase, Object value) {
            events.add(phase);
            if (phase >= 10) {
                ++stages;
                require(before == null && Trace.core == null && value == result(), "conditional argument effect");
                if (mode == 15 || mode == 16 || mode == 22 && stages == 2)
                    PairedConstructorConditionsHarness.<RuntimeException>throwAny(failure);
            } else if (phase == 2) {
                require(before == null && Trace.core == null && value instanceof Core && ((Core)value).value == null,
                        "callback before final assignment");
                before = (Core)value;
                if (mode == 17 || mode == 18) PairedConstructorConditionsHarness.<RuntimeException>throwAny(failure);
            } else {
                require(phase == 3 && value == before && value == Trace.core && before.value == result(),
                        "final store and escaped receiver");
                if (mode == 19 || mode == 20) PairedConstructorConditionsHarness.<RuntimeException>throwAny(failure);
            }
        }
        void verify(Core answer, Throwable caught) {
            List<Integer> expected = new ArrayList<Integer>();
            boolean nullFactory = mode == 12, stepFailed = mode == 5 || mode == 13 || mode == 14;
            boolean stageFailed = mode == 15 || mode == 16 || mode == 22;
            if (!nullFactory) {
                int count = stepFailed ? mode == 5 ? 3 : 1 : (int)iterations();
                for (int i = 0; i < count; ++i) expected.add(1);
                if (!stepFailed) {
                    int comparison = Integer.compare(start, bound);
                    int[] phases = {comparison == 0 ? 10 : 11, comparison < 0 ? 12 : 13,
                            comparison > 0 ? 14 : 15, left == right ? 16 : 17};
                    for (int i = 0; i < phases.length; ++i) {
                        expected.add(phases[i]);
                        if (stageFailed && i == (mode == 22 ? 1 : 0)) break;
                    }
                    if (!stageFailed) { expected.add(2); if (mode != 17 && mode != 18) expected.add(3); }
                }
            }
            require(events.equals(expected), "ordered effects " + events + " expected " + expected);
            boolean callbackFailed = stepFailed || stageFailed || mode >= 17 && mode <= 20;
            if (nullFactory) require(caught != null && caught.getClass() == NullPointerException.class, "original null invocation");
            else require(caught == (callbackFailed ? failure : null), "checked and Error identity");
            boolean failed = nullFactory || callbackFailed;
            require((answer == null) == failed && (answer == null || answer == before), "constructor result identity");
            boolean entered = !nullFactory && !stepFailed && !stageFailed;
            require((before != null) == entered, "delegation boundary");
            if (before != null) {
                boolean partial = mode == 17 || mode == 18;
                require(before.value == (partial ? null : result()), "partial final assignment");
                require(Trace.core == (partial ? null : before), "partial receiver escape");
            } else require(Trace.core == null, "no early receiver escape");
        }
    }
    private static Core compiledCaller(Factory factory, int start, int bound, Object left, Object right) {
        return new Core(factory, start, bound, left, right);
    }
    private static Core call(int route, Factory factory, int start, int bound, Object left, Object right) throws Throwable {
        if (route == 0) {
            try { return Core.class.getConstructor(Factory.class, int.class, int.class, Object.class, Object.class)
                    .newInstance(factory, start, bound, left, right); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (route == 1) {
            MethodHandle handle = MethodHandles.publicLookup().findConstructor(Core.class,
                    MethodType.methodType(void.class, Factory.class, int.class, int.class, Object.class, Object.class));
            return (Core)handle.invokeExact(factory, start, bound, left, right);
        }
        return compiledCaller(factory, start, bound, left, right);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; ++attempt) for (int repeat = 0; repeat < (mode == 21 ? 2 : 1); ++repeat) {
            Observer observer = new Observer(mode); Core answer = null; Throwable failure = null;
            try { answer = call(route, mode == 12 ? null : observer, observer.start, observer.bound, observer.left, observer.right); }
            catch (Throwable caught) { failure = caught; }
            observer.verify(answer, failure);
        }
        System.out.println("attempts=2,paired-constructor-conditions=matched");
    }
}
