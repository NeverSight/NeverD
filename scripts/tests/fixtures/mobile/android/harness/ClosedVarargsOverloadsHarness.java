import fixture.Core;
import fixture.Leaf;
import fixture.Trace;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;

public final class ClosedVarargsOverloadsHarness {
    private static final class Checked extends Exception {}
    private static Core previous;
    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError("owned closed varargs overloads: " + reason);
    }
    private static <E extends Throwable> void throwAny(Throwable failure) throws E { throw (E)failure; }
    private static final class PoisonList extends ArrayList<Object> {
        public int size() { throw new AssertionError("owned closed varargs overloads: size observed"); }
        public Object get(int index) { throw new AssertionError("owned closed varargs overloads: element observed"); }
        public Iterator<Object> iterator() { throw new AssertionError("owned closed varargs overloads: iterator observed"); }
        public Object[] toArray() { throw new AssertionError("owned closed varargs overloads: copy observed"); }
        public boolean equals(Object value) { throw new AssertionError("owned closed varargs overloads: equality observed"); }
    }
    private static final class Observer extends Trace {
        final int mode, family;
        final Object input;
        final Throwable failure;
        final List<Integer> events = new ArrayList<Integer>();
        Core before;
        Observer(int mode) {
            this.mode = mode;
            family = mode >= 21 || mode >= 7 && mode <= 10 ? 3 : mode >= 12 && mode <= 14 || mode == 19 || mode == 20 ? 2 : mode >= 15 && mode <= 18 ? 1 : 0;
            if (mode == 1 || mode == 13 || mode == 16 || mode == 22) input = null;
            else if (mode == 2 || mode == 23) input = new Object();
            else if (mode == 3 || family == 2) input = mode == 14 ? new Leaf[] {null, new Leaf()} : new Leaf[] {new Leaf(), new Leaf()};
            else if (mode == 6) input = new PoisonList();
            else {
                List<Object> list = new ArrayList<Object>();
                list.add(mode == 4 || mode == 17 ? new Object() : mode == 5 ? null : new Leaf());
                list.add(new Leaf()); input = list;
            }
            failure = mode == 8 || mode == 10 || mode == 19 ? new AssertionError("callback") : new Checked();
            Trace.hook = this; Trace.core = null;
        }
        public void stage(int phase, Object value) {
            events.add(phase);
            int first = family == 2 ? 4 : 2;
            if (phase == first) {
                require(before == null && Trace.core == null && value instanceof Core && value != previous
                        && ((Core)value).value == null, "selected constructor before final store");
                before = (Core)value;
                if (mode == 7 || mode == 8 || mode == 18) ClosedVarargsOverloadsHarness.<RuntimeException>throwAny(failure);
            } else {
                require(phase == first + 1 && value == before && value == Trace.core && before.value == input,
                        "selected overload, argument and receiver identity");
                if (mode == 9 || mode == 10 || mode == 19) ClosedVarargsOverloadsHarness.<RuntimeException>throwAny(failure);
            }
        }
        void verify(Core answer, Throwable caught) {
            boolean castFailed = mode == 2 || mode == 3 || mode == 23;
            boolean beforeFailed = mode == 7 || mode == 8 || mode == 18;
            boolean afterFailed = mode == 9 || mode == 10 || mode == 19;
            List<Integer> expected = new ArrayList<Integer>();
            int first = family == 2 ? 4 : 2;
            if (!castFailed) { expected.add(first); if (!beforeFailed) expected.add(first + 1); }
            require(events.equals(expected), "ordered overload effects " + events + " expected " + expected);
            if (castFailed) require(caught != null && caught.getClass() == ClassCastException.class, "cast before delegation");
            else require(caught == (beforeFailed || afterFailed ? failure : null), "original checked and Error identity");
            require((answer == null) == (castFailed || beforeFailed || afterFailed)
                    && (answer == null || answer == before), "constructor result identity");
            require((before != null) == !castFailed, "delegation boundary");
            if (before != null) {
                require(before.value == (beforeFailed ? null : input), "partial final assignment");
                require(Trace.core == (beforeFailed ? null : before), "partial receiver escape");
                previous = before;
            } else require(Trace.core == null, "no early receiver escape");
        }
    }
    private static Core compiledCaller(int family, Object input, int marker) {
        if (family == 3) return new Core(input, (Leaf[])null);
        if (family == 2) return new Core((Leaf[])input);
        if (family == 1) return new Core((List<Leaf>)input);
        return new Core(input, marker);
    }
    private static Core call(int route, int family, Object input, int marker) throws Throwable {
        if (route == 0) {
            try {
                if (family == 3) return Core.class.getConstructor(Object.class, Leaf[].class).newInstance(new Object[] {input, null});
                if (family == 2) return Core.class.getConstructor(Leaf[].class).newInstance(new Object[] {input});
                if (family == 1) return Core.class.getConstructor(List.class).newInstance(new Object[] {input});
                return Core.class.getConstructor(Object.class, int.class).newInstance(input, marker);
            } catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (route == 1) {
            if (family == 3) {
                MethodHandle handle = MethodHandles.publicLookup().findConstructor(Core.class,
                        MethodType.methodType(void.class, Object.class, Leaf[].class)).asFixedArity();
                return (Core)handle.invokeExact(input, (Leaf[])null);
            }
            if (family == 2) {
                MethodHandle handle = MethodHandles.publicLookup().findConstructor(Core.class,
                        MethodType.methodType(void.class, Leaf[].class)).asFixedArity();
                return (Core)handle.invokeExact((Leaf[])input);
            }
            if (family == 1) {
                MethodHandle handle = MethodHandles.publicLookup().findConstructor(Core.class,
                        MethodType.methodType(void.class, List.class));
                return (Core)handle.invokeExact((List)input);
            }
            MethodHandle handle = MethodHandles.publicLookup().findConstructor(Core.class,
                    MethodType.methodType(void.class, Object.class, int.class));
            return (Core)handle.invokeExact(input, marker);
        }
        return compiledCaller(family, input, marker);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        require(Core.class.getConstructor(Leaf[].class).isVarArgs(), "original varargs flag");
        require(Core.class.getConstructor(Object.class, Leaf[].class).isVarArgs(), "original varargs caller flag");
        require(!Core.class.getConstructor(Object.class, int.class).isVarArgs(), "fixed caller flag");
        require(Core.class.getConstructor(List.class).getGenericParameterTypes()[0].getTypeName()
                .equals("java.util.List<fixture.Leaf>"), "original generic parameter");
        require(Modifier.isFinal(Core.class.getField("value").getModifiers()), "original final field");
        for (int attempt = 0; attempt < 2; ++attempt) for (int repeat = 0; repeat < (mode == 11 || mode == 20 ? 2 : 1); ++repeat) {
            Observer observer = new Observer(mode); Core answer = null; Throwable failure = null;
            try { answer = call(route, observer.family, observer.input, mode); }
            catch (Throwable caught) { failure = caught; }
            observer.verify(answer, failure);
        }
        System.out.println("attempts=2,closed-varargs-overloads=matched");
    }
}
