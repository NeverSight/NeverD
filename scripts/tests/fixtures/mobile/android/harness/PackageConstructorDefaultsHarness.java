package fixture;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Modifier;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.lang.reflect.WildcardType;
import java.util.ArrayList;
import java.util.List;

public final class PackageConstructorDefaultsHarness {
    private static final class Checked extends Exception {}
    private static final class Inert implements Carrier<Object> { public Object current() { return null; } }
    private static final class Unobserved {
        public String toString() { throw new AssertionError("owned package constructor defaults: marker observed"); }
        public boolean equals(Object value) { throw new AssertionError("owned package constructor defaults: marker compared"); }
    }
    private static Core previous;
    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError("owned package constructor defaults: " + reason);
    }
    private static <E extends Throwable> void throwAny(Throwable failure) throws E { throw (E)failure; }
    private static final class Observer extends Trace implements Carrier<Object> {
        final int mode, mask;
        final Carrier inputState;
        final Core inputNext, expectedNext;
        final Object marker;
        final Throwable failure;
        final List<Integer> events = new ArrayList<Integer>();
        Object currentValue;
        Core before;
        int reads;
        Observer(int mode) {
            this.mode = mode;
            mask = mode == 1 || mode == 6 || mode == 9 || mode == 21 ? 2 : mode == 2 ? 1
                    : mode == 3 ? 3 : mode == 4 ? Integer.MIN_VALUE : mode == 5 ? Integer.MAX_VALUE : mode == 23 ? -3 : 0;
            Trace.hook = new Trace();
            Core prior = new Core(new Inert(), null);
            inputNext = mode == 6 || mode == 7 ? null : prior;
            expectedNext = (mask & 2) != 0 ? null : inputNext;
            inputState = mode == 8 || mode == 9 ? null : this;
            currentValue = mode == 10 ? null : new Object();
            marker = mode == 22 ? new Unobserved() : null;
            failure = mode == 12 || mode == 14 || mode == 16 || mode == 18 || mode == 20
                    ? new AssertionError("callback") : new Checked();
            Trace.core = null; Trace.hook = this;
        }
        public Object current() {
            events.add(1); ++reads;
            require(reads == 1 && before != null && before.state == inputState && before.next == expectedNext
                    && before.initial == null && Trace.core == null, "getter after both original field stores");
            if (mode == 11 || mode == 12) PackageConstructorDefaultsHarness.<RuntimeException>throwAny(failure);
            return currentValue;
        }
        public void stage(int phase, Object value) {
            events.add(phase);
            if (phase == 2) {
                require(before == null && Trace.core == null && value instanceof Core && value != previous,
                        "fresh package receiver before field stores");
                before = (Core)value;
                require(before.state == null && before.next == null && before.initial == null && reads == 0,
                        "all final defaults before first callback");
                if (mode == 13 || mode == 14) PackageConstructorDefaultsHarness.<RuntimeException>throwAny(failure);
            } else if (phase == 3) {
                require(value == before && before.state == inputState && before.next == null && before.initial == null
                        && reads == 0 && Trace.core == null, "state before next field");
                if (mode == 15 || mode == 16) PackageConstructorDefaultsHarness.<RuntimeException>throwAny(failure);
            } else if (phase == 4) {
                require(value == before && before.state == inputState && before.next == expectedNext
                        && before.initial == null && reads == 0 && Trace.core == null, "next before state getter");
                if (mode == 23) currentValue = new Object();
                if (mode == 17 || mode == 18) PackageConstructorDefaultsHarness.<RuntimeException>throwAny(failure);
            } else {
                require(phase == 5 && value == before && value == Trace.core && before.state == inputState
                        && before.next == expectedNext && before.initial == currentValue && reads == 1,
                        "initial result and escaped receiver");
                if (mode == 19 || mode == 20) PackageConstructorDefaultsHarness.<RuntimeException>throwAny(failure);
            }
        }
        void verify(Core answer, Throwable caught) {
            boolean firstFailed = mode == 13 || mode == 14, stateFailed = mode == 15 || mode == 16;
            boolean nextFailed = mode == 17 || mode == 18, getterFailed = mode == 11 || mode == 12;
            boolean finalFailed = mode == 19 || mode == 20, nullState = inputState == null;
            List<Integer> expected = new ArrayList<Integer>(); expected.add(2);
            if (!firstFailed) {
                expected.add(3);
                if (!stateFailed) {
                    expected.add(4);
                    if (!nextFailed && !nullState) { expected.add(1); if (!getterFailed) expected.add(5); }
                }
            }
            require(events.equals(expected), "ordered effects " + events + " expected " + expected);
            if (nullState) require(caught != null && caught.getClass() == NullPointerException.class, "null state getter after stores");
            else require(caught == (mode >= 11 && mode <= 20 ? failure : null), "checked and Error identity");
            boolean failed = nullState || mode >= 11 && mode <= 20;
            require((answer == null) == failed && (answer == null || answer == before), "constructor result identity");
            require(before != null && before != previous, "delegation receiver");
            require(before.state == (firstFailed ? null : inputState), "partial state final");
            require(before.next == (firstFailed || stateFailed ? null : expectedNext), "partial next final");
            boolean completedGetter = !firstFailed && !stateFailed && !nextFailed && !nullState && !getterFailed;
            require(before.initial == (completedGetter ? currentValue : null), "partial initial final");
            require(Trace.core == (completedGetter ? before : null), "partial receiver escape");
            require(reads == (!firstFailed && !stateFailed && !nextFailed && !nullState ? 1 : 0), "single state read");
            previous = before;
        }
    }
    private static Core compiledCaller(Carrier state, Core next, int mask, Object marker) {
        return new Core(state, next, mask, marker);
    }
    private static Core call(int route, Carrier state, Core next, int mask, Object marker) throws Throwable {
        if (route == 0) {
            try { return Core.class.getDeclaredConstructor(Carrier.class, Core.class, int.class, Object.class)
                    .newInstance(state, next, mask, marker); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (route == 1) {
            MethodHandle handle = MethodHandles.lookup().findConstructor(Core.class,
                    MethodType.methodType(void.class, Carrier.class, Core.class, int.class, Object.class));
            return (Core)handle.invokeExact(state, next, mask, marker);
        }
        return compiledCaller(state, next, mask, marker);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        require(!Modifier.isPublic(Core.class.getModifiers()) && Modifier.isFinal(Core.class.getModifiers()),
                "original package class visibility");
        Type[] parameters = Core.class.getDeclaredConstructor(Carrier.class, Core.class).getGenericParameterTypes();
        require(parameters[1] == Core.class && parameters[0] instanceof ParameterizedType, "closed self and carrier parameters");
        ParameterizedType carrier = (ParameterizedType)parameters[0];
        require(carrier.getRawType() == Carrier.class && carrier.getActualTypeArguments()[0] instanceof WildcardType,
                "original carrier variance");
        ParameterizedType field = (ParameterizedType)Core.class.getDeclaredField("state").getGenericType();
        require(field.getRawType() == Carrier.class && field.getActualTypeArguments()[0] == Object.class,
                "original invariant state field");
        for (int attempt = 0; attempt < 2; ++attempt) for (int repeat = 0; repeat < (mode == 21 ? 2 : 1); ++repeat) {
            Observer observer = new Observer(mode); Core answer = null; Throwable failure = null;
            try { answer = call(route, observer.inputState, observer.inputNext, observer.mask, observer.marker); }
            catch (Throwable caught) { failure = caught; }
            observer.verify(answer, failure);
        }
        System.out.println("attempts=2,package-constructor-defaults=matched");
    }
}
