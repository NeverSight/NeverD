package fixture;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.List;

public final class GuardedDelegationTailHarness {
    private static final class Checked extends Exception {}
    private static final class Fault extends Error {}
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned guarded delegation tail mismatch");
    }
    @SuppressWarnings("unchecked")
    private static <E extends Throwable> RuntimeException forward(Throwable failure) throws E {
        throw (E) failure;
    }
    private static final class Child extends Core {
        Child(Context context, int salt, long stamp, Object[] values) throws Throwable {
            super(context, salt, stamp, values);
        }
        @Override public Core accept(Context context, int index, Object value) throws Throwable {
            Observer observer = (Observer) context;
            observer.overrides++;
            observer.step(this, index, value);
            return null;
        }
    }
    private static final class Observer implements Context {
        final int route, mode, salt, seed;
        final boolean branch;
        final long stamp;
        final Object first = new Object(), second = new Object(), last = new Object();
        final Object replacement = new Object();
        final Object[] values;
        final Throwable failure;
        final List<Integer> events = new ArrayList<Integer>();
        int steps, overrides;
        Observer(int route, boolean branch, int mode, int attempt) {
            this.route = route; this.mode = mode; this.branch = branch;
            this.salt = mode == 20 ? Integer.MAX_VALUE - 5 : mode == 22 ? Integer.MIN_VALUE : 31 + attempt;
            this.seed = branch ? salt : salt + 47;
            this.stamp = attempt == 0 ? 0x1234567887654321L : 0xfedcba9813572468L;
            this.values = mode == 15 ? null : mode == 0 ? new Object[0]
                    : mode == 1 ? new Object[]{first} : new Object[]{first, second, last};
            if (mode == 21) { values[0] = null; values[1] = values; }
            this.failure = (mode & 1) == 1 ? new Checked() : new Fault();
        }
        void input(Object[] actual) { require(actual == values); }
        void fields(Core self) {
            require(self != null && self == Trace.leaked && self.seed == seed && self.stamp == stamp);
        }
        public boolean choose(Object[] actual) {
            input(actual); require(events.isEmpty() && Trace.leaked == null); events.add(1);
            if (mode == 3 || mode == 4) throw GuardedDelegationTailHarness.<RuntimeException>forward(failure);
            if (mode == 18) values[0] = replacement;
            if (mode == 19) Trace.context = null;
            return branch;
        }
        public int offset(Object[] actual) {
            input(actual); require(!branch && events.size() == 1 && Trace.leaked == null); events.add(2);
            if (mode == 18) require(actual[0] == replacement);
            if (mode == 5 || mode == 6) throw GuardedDelegationTailHarness.<RuntimeException>forward(failure);
            return 47;
        }
        public void primary(Core self, int value, long number) throws Throwable {
            fields(self); require(value == seed && number == stamp && events.size() == (branch ? 1 : 2));
            events.add(3);
            if (mode == 7 || mode == 8) throw failure;
            if (mode == 16) values[1] = replacement;
        }
        public void step(Core self, int index, Object value) throws Throwable {
            fields(self); require(index == steps && value == values[index]);
            require(overrides == (route == 3 ? index + 1 : 0));
            require(events.size() == (branch ? 2 : 3) + index);
            events.add(10 + index); steps++;
            if (mode == 9 || mode == 10 || (mode == 11 || mode == 12) && index == 1) throw failure;
            if (mode == 17 && index == 0) {
                values[1] = replacement;
                Trace.input = new Object[0];
            }
        }
        public void finish(Core self, Object[] actual) throws Throwable {
            fields(self); input(actual); require(steps == values.length); events.add(4);
            if (mode == 13 || mode == 14) throw failure;
        }
        void verify(Core result, Throwable caught) {
            boolean prefixFailure = mode == 3 || mode == 4 || !branch && (mode == 5 || mode == 6);
            boolean primaryFailure = mode == 7 || mode == 8;
            boolean stepFailure = mode >= 9 && mode <= 12;
            boolean finishFailure = mode == 13 || mode == 14;
            boolean nullFailure = mode == 15 || mode == 23;
            require(caught == (prefixFailure || primaryFailure || stepFailure || finishFailure ? failure : null)
                    || nullFailure && caught != null && caught.getClass() == NullPointerException.class);
            if (nullFailure) require(caught != null && caught.getClass() == NullPointerException.class);
            if (prefixFailure || mode == 23) require(Trace.leaked == null);
            else fields(Trace.leaked);
            require(result == (caught == null ? Trace.leaked : null));
            List<Integer> wanted = new ArrayList<Integer>();
            if (mode != 23) {
                wanted.add(1);
                if (mode != 3 && mode != 4) {
                    if (!branch) wanted.add(2);
                    if (!prefixFailure) {
                        wanted.add(3);
                        if (!primaryFailure && mode != 15) {
                            int count = mode == 9 || mode == 10 ? 1 : mode == 11 || mode == 12 ? 2 : values.length;
                            for (int index = 0; index < count; ++index) wanted.add(10 + index);
                            if (!stepFailure) wanted.add(4);
                        }
                    }
                }
            }
            require(events.equals(wanted));
            require(overrides == (route == 3 ? steps : 0));
            if (mode == 17) require(Trace.input != values && Trace.input.length == 0);
            if (mode == 19) require(Trace.context == null);
        }
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), branch = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        require(Core.class.getConstructor(Context.class, int.class, long.class, Object[].class).isVarArgs());
        require(Modifier.isFinal(Core.class.getField("seed").getModifiers()));
        require(Modifier.isFinal(Core.class.getField("stamp").getModifiers()));
        for (int attempt = 0; attempt < 2; ++attempt) {
            Observer observer = new Observer(route, branch != 0, mode, attempt);
            Trace.leaked = null; Trace.input = observer.values; Trace.context = observer;
            Core value = null; Throwable caught = null;
            Context context = mode == 23 ? null : observer;
            try {
                if (route == 0) value = new Core(context, observer.salt, observer.stamp, observer.values);
                else if (route == 1) value = Core.class.getConstructor(Context.class, int.class, long.class, Object[].class)
                        .newInstance(context, observer.salt, observer.stamp, observer.values);
                else if (route == 2) value = (Core) MethodHandles.lookup().findConstructor(Core.class,
                        MethodType.methodType(void.class, Context.class, int.class, long.class, Object[].class))
                        .asFixedArity().invokeExact(context, observer.salt, observer.stamp, observer.values);
                else value = new Child(context, observer.salt, observer.stamp, observer.values);
            } catch (InvocationTargetException ex) { caught = ex.getCause(); }
              catch (Throwable ex) { caught = ex; }
            observer.verify(value, caught);
        }
        System.out.println("attempts=2,guarded-delegation-tail=matched");
    }
}
