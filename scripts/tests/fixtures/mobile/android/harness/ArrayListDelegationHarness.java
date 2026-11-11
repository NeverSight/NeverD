import fixture.*;
import java.util.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ArrayListDelegationHarness {
    static void check(boolean value) { if (!value) throw new AssertionError("owned array list delegation"); }
    static <E extends Throwable> void raise(Throwable value) throws E { throw (E)value; }
    static final class Checked extends Exception {}
    static final class Poison extends ArrayList<Object> {
        public Object get(int i) { throw new AssertionError("early list read"); }
        public int size() { throw new AssertionError("early list size"); }
        public Iterator<Object> iterator() { throw new AssertionError("early list iteration"); }
        public Object[] toArray() { throw new AssertionError("early list copy"); }
    }
    static final class Observer extends Trace {
        final int mode, family;
        final Config expected;
        final Carrier replacement = new Carrier();
        final Carrier[] array;
        final List list;
        final Throwable failure;
        int events;
        Core before;
        Observer(int family, int mode, Config config) {
            this.family = family; this.mode = mode; expected = config;
            array = mode == 1 ? null : mode == 2 ? new Carrier[0] : new Carrier[] {new Carrier(), null};
            list = mode == 1 ? null : mode == 6 ? new Poison() : new ArrayList();
            if (list != null && mode != 6) list.add(mode == 7 ? new Object() : new Carrier());
            failure = mode == 4 ? new AssertionError("owned error") : mode == 5 ? new Checked() : new RuntimeException("owned failure");
        }
        public void hit(int phase, Core core) {
            events = events * 10 + phase;
            check(!core.finished);
            if (phase == 1) {
                before = core;
                check(core.config == null && core.values == null && Trace.latest == null);
                Config.DEFAULT = new Config();
            } else {
                check(core == before && Trace.latest == core && core.config == expected);
                if (phase == 2) {
                    check(core.values == null);
                    if (array != null && array.length != 0) array[0] = replacement;
                } else if (family >= 2) check(core.values == list);
            }
            if (phase == (mode == 3 ? 1 : mode == 4 ? 2 : mode == 5 ? 3 : 0))
                ArrayListDelegationHarness.<RuntimeException>raise(failure);
        }
    }
    static Core call(int route, int family, Observer o) throws Throwable {
        if (route == 0) {
            if (family == 0) return new Core(o.expected, o.array);
            if (family == 1) return new Core(o.array);
            if (family == 2) return new Core(o.expected, o.list);
            return new Core(o.list);
        }
        Class<?> input = family < 2 ? Carrier[].class : List.class;
        Class<?>[] parameters = family % 2 == 0 ? new Class<?>[] {Config.class, input} : new Class<?>[] {input};
        Object[] arguments = family % 2 == 0 ? new Object[] {o.expected, family < 2 ? o.array : o.list} : new Object[] {family < 2 ? o.array : o.list};
        if (route == 1) {
            try { return Core.class.getConstructor(parameters).newInstance(arguments); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        MethodHandle target = MethodHandles.publicLookup().findConstructor(Core.class, MethodType.methodType(void.class, parameters)).asFixedArity();
        return (Core)target.invokeWithArguments(arguments);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), family = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        check(Core.class.getConstructor(Carrier[].class).isVarArgs());
        check(Core.class.getConstructor(Config.class, Carrier[].class).isVarArgs());
        check(!Core.class.getConstructor(Config.class, List.class).isVarArgs());
        check(Core.class.getGenericSuperclass().getTypeName().equals("fixture.Parent<fixture.Bound>"));
        Core previous = null;
        for (int attempt = 0; attempt < 2; attempt++) {
            Config original = new Config(); Config.DEFAULT = original;
            Observer o = new Observer(family, mode, original);
            Trace.hook = o; Trace.latest = null;
            Core answer = null; Throwable problem = null;
            try { answer = call(route, family, o); } catch (Throwable failure) { problem = failure; }
            if (family < 2 && mode == 1) {
                check(problem instanceof NullPointerException && o.events == 0 && o.before == null && Trace.latest == null);
            } else if (mode >= 3 && mode <= 5) {
                check(problem == o.failure && answer == null && o.before != null && !o.before.finished);
                check(o.events == (mode == 3 ? 1 : mode == 4 ? 12 : 123));
            } else {
                check(problem == null && answer == o.before && answer != previous && answer.finished && o.events == 123);
                check(answer.config == original && Trace.latest == answer);
                if (family < 2) {
                    List values = answer.values;
                    check(values.size() == o.array.length);
                    if (o.array.length != 0) {
                        check(values.get(0) == o.replacement);
                        Carrier next = new Carrier(); values.set(0, next); check(o.array[0] == next);
                        try { values.set(0, new Object()); throw new AssertionError("missing array store"); }
                        catch (ArrayStoreException expected) {}
                    }
                    try { values.add(new Carrier()); throw new AssertionError("missing fixed size"); }
                    catch (UnsupportedOperationException expected) {}
                } else {
                    check(answer.values == o.list);
                    if (mode == 7) {
                        try { Carrier value = answer.values.get(0); throw new AssertionError("missing late cast"); }
                        catch (ClassCastException expected) { check(o.events == 123 && answer.finished); }
                    }
                }
                previous = answer;
            }
        }
        System.out.println("attempts=2,array-list-delegation=matched");
    }
}
