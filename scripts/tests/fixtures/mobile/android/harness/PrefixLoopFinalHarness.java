package fixture;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.List;

public final class PrefixLoopFinalHarness {
    private static final class Checked extends Exception {}
    private static final class Fault extends Error {}
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned prefix loop final mismatch");
    }
    private static final class Observer implements Context {
        final int mode, rounds, prefix;
        final boolean requested, selected;
        final long seed, tail;
        final Object[] token = {new Object(), new Object()};
        final Object replacement = new Object();
        final Object payload;
        final Throwable failure;
        final List<Integer> events = new ArrayList<Integer>();
        int steps;
        Core escaped;
        volatile boolean spinning, completed;
        volatile Throwable workerFailure;
        Observer(int mode, boolean branch, long seed) {
            this.mode = mode; this.requested = branch; this.selected = mode == 19 ? !branch : branch;
            this.seed = seed; this.tail = seed ^ 0x6a5b4c3d2e1f7081L;
            this.rounds = mode == 24 ? Integer.MIN_VALUE : mode == 0 ? 0 : mode == 1 ? 1 : mode == 22 ? -1 : 3;
            this.prefix = mode == 21 ? Integer.MIN_VALUE : (int)(seed ^ (seed >>> 32));
            this.payload = mode == 16 ? null : mode == 17 ? token : new Object();
            this.failure = (mode & 1) == 0 ? new Checked() : new Fault();
        }
        void check(Core self, int stage) {
            require(self != null && self == Trace.leaked);
            if (escaped == null) escaped = self;
            require(self == escaped);
            require(self.prefix == (stage >= 1 ? prefix : 0));
            require(self.choice == (stage >= 2 && selected));
            require(self.payload == (stage >= 3 ? payload : null));
            require(self.tail == (stage >= 4 ? tail : 0L));
        }
        void seed(long value, Object object) { require(value == seed && object == token); }
        public int enter(Core self, long value, Object object) throws Throwable {
            check(self, 0); seed(value, object); require(events.isEmpty()); events.add(1);
            if (mode == 4 || mode == 5) throw failure;
            return prefix;
        }
        public void step(Core self, int index, Object object) throws Throwable {
            check(self, 1); require(object == token && index == steps && events.size() == steps + 1);
            events.add(10 + index); steps++;
            if (mode == 6 || mode == 7 || (mode == 8 || mode == 9) && index == 1) throw failure;
            if (mode == 18 && index == 0) token[0] = replacement;
            if (mode == 20 && index == 0) Trace.token = replacement;
            if (mode == 23 && index == 0) Trace.rounds = 0;
        }
        public void spin(Core self, Object object) throws Throwable {
            check(self, 1); require(mode == 24 && object == token && steps == 0);
            require(events.size() == 1 && events.get(0) == 1); events.add(6);
            spinning = true;
        }
        public boolean choose(Core self, boolean branch) throws Throwable {
            check(self, 1); require(branch == requested && steps == Math.max(rounds, 0));
            require(events.size() == steps + 1); events.add(2);
            if (mode == 10 || mode == 11) throw failure;
            return selected;
        }
        private Object value(Core self, long number, Object object, boolean left) throws Throwable {
            check(self, 2); seed(number, object); require(left == selected);
            require(events.size() == steps + 2 && events.get(events.size() - 1) == 2);
            events.add(left ? 3 : 4);
            if (mode == 12 || mode == 13) throw failure;
            return payload;
        }
        public Object left(Core self, long number, Object object) throws Throwable { return value(self, number, object, true); }
        public Object right(Core self, long number, Object object) throws Throwable { return value(self, number, object, false); }
        public long finish(Core self, long number) throws Throwable {
            check(self, 3); require(number == seed && events.size() == steps + 3);
            events.add(5);
            if (mode == 14 || mode == 15) throw failure;
            return tail;
        }
        void verify(Core result, Throwable caught) {
            int stage = mode == 3 || mode == 4 || mode == 5 ? 0
                    : mode >= 6 && mode <= 11 ? 1 : mode == 12 || mode == 13 ? 2
                    : mode == 14 || mode == 15 ? 3 : 4;
            check(Trace.leaked, stage);
            if (mode == 3) require(caught != null && caught.getClass() == NullPointerException.class);
            else require(caught == (mode >= 4 && mode <= 15 ? failure : null));
            require(result == (caught == null ? escaped : null));
            List<Integer> wanted = new ArrayList<Integer>();
            if (mode != 3) wanted.add(1);
            int expectedSteps = mode == 3 || mode == 4 || mode == 5 ? 0
                    : mode == 6 || mode == 7 ? 1 : mode == 8 || mode == 9 ? 2 : Math.max(rounds, 0);
            for (int i = 0; i < expectedSteps; ++i) wanted.add(10 + i);
            if (!(mode >= 3 && mode <= 9)) wanted.add(2);
            if (!(mode >= 3 && mode <= 11)) wanted.add(selected ? 3 : 4);
            if (!(mode >= 3 && mode <= 13)) wanted.add(5);
            require(steps == expectedSteps && events.equals(wanted));
            if (mode == 18) require(token[0] == replacement);
            require(Trace.token == (mode == 20 ? replacement : token));
            require(Trace.rounds == (mode == 23 ? 0 : rounds));
        }
    }
    private static Core direct(Context context, Observer o) throws Throwable {
        return new Core(context, o.seed, o.rounds, o.requested, o.token);
    }
    private static Core construct(int route, Context context, Observer o) throws Throwable {
        if (route == 0) return direct(context, o);
        if (route == 1) {
            try {
                return Core.class.getConstructor(Context.class, long.class, int.class, boolean.class, Object.class)
                        .newInstance(context, o.seed, o.rounds, o.requested, o.token);
            } catch (InvocationTargetException error) { throw error.getCause(); }
        }
        return (Core)MethodHandles.publicLookup().findConstructor(Core.class,
                MethodType.methodType(void.class, Context.class, long.class, int.class, boolean.class, Object.class))
                .invokeWithArguments(context, o.seed, o.rounds, o.requested, (Object)o.token);
    }
    private static void observeSpin(final int route, final Observer o) throws Throwable {
        Thread worker = new Thread(new Runnable() {
            public void run() {
                try { construct(route, o, o); }
                catch (Throwable error) { o.workerFailure = error; }
                finally { o.completed = true; }
            }
        });
        worker.setDaemon(true);
        worker.start();
        long deadline = System.nanoTime() + 2000000000L;
        while (!o.spinning && !o.completed && System.nanoTime() < deadline) Thread.sleep(1);
        require(o.spinning && !o.completed);
        worker.join(200);
        require(worker.isAlive() && !o.completed && o.workerFailure == null);
        o.check(Trace.leaked, 1);
        require(o.steps == 0 && o.events.size() == 2 && o.events.get(0) == 1 && o.events.get(1) == 6);
        require(Trace.token == o.token && Trace.rounds == Integer.MIN_VALUE);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), branch = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        require(route >= 0 && route < 3 && branch >= 0 && branch < 2 && mode >= 0 && mode < 25);
        for (String field : new String[]{"prefix", "choice", "payload", "tail"})
            require(Modifier.isFinal(Core.class.getDeclaredField(field).getModifiers()));
        for (int attempt = 0; attempt < 2; ++attempt) {
            Observer o = new Observer(mode, branch != 0,
                    attempt == 0 ? Long.MIN_VALUE + 0x12345678L : Long.MAX_VALUE - 0x76543210L);
            Trace.leaked = null; Trace.token = o.token; Trace.rounds = o.rounds;
            Context context = mode == 3 ? null : o;
            if (mode == 24) { observeSpin(route, o); continue; }
            Core result = null; Throwable caught = null;
            try { result = construct(route, context, o); }
            catch (Throwable error) { caught = error; }
            o.verify(result, caught);
        }
        System.out.println("attempts=2,prefix-loop-final=matched");
    }
}
