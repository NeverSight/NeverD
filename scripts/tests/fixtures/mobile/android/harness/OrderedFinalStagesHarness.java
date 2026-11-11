import fixture.Context;
import fixture.Core;
import fixture.Trace;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

public final class OrderedFinalStagesHarness {
    private static final class Checked extends Exception {}

    private static void require(boolean condition, String reason) {
        if (!condition) throw new AssertionError("owned ordered final stages: " + reason);
    }

    private static final class Observer implements Context {
        final int mode;
        final int selector;
        final long seed;
        final Object[] token = {new Object(), new Object()};
        final Object replacement = new Object();
        final Object payload;
        final Object marker;
        final int prefix;
        final long tail;
        final Throwable bodyFailure;
        final Throwable cleanupFailure;
        final List<String> events = new ArrayList<String>();
        Core escaped;
        int cleanupStage = 1;
        int cleanupCount;
        int valueCount;

        Observer(int mode, int selector, long seed) {
            this.mode = mode;
            this.selector = selector;
            this.seed = seed;
            this.prefix = (int)(seed ^ (seed >>> 32));
            this.tail = seed ^ 0x6a5b4c3d2e1f7081L;
            this.payload = mode == 14 ? null : mode == 17 ? token : new Object[] {token, replacement};
            this.marker = mode == 15 ? null : mode == 17 ? token : new Object();
            this.bodyFailure = (mode == 2 || mode == 4 || mode == 6 || mode == 11 || mode == 13)
                    ? new AssertionError("body") : new Checked();
            this.cleanupFailure = (mode == 8 || mode == 18)
                    ? new AssertionError("cleanup") : new Checked();
        }

        void check(Core self, int stage) {
            require(self != null && Trace.leaked == self, "escaped identity");
            if (escaped == null) escaped = self;
            require(escaped == self, "callback receiver changed");
            require(self.prefix == (stage >= 1 ? prefix : 0), "prefix visibility at " + stage);
            require(self.choice == (stage >= 2 && selector != 0), "choice visibility at " + stage);
            require(self.payload == (stage >= 3 ? payload : null), "payload visibility at " + stage);
            require(self.tail == (stage >= 4 ? tail : 0L), "wide visibility at " + stage);
            require(self.marker == (stage >= 5 ? marker : null), "marker visibility at " + stage);
        }

        void seed(long value, Object object) {
            require(value == seed && object == token, "wide or object argument changed");
        }

        public int enter(Core self, long value, Object object) throws Throwable {
            require(events.isEmpty(), "repeated enter");
            events.add("enter"); check(self, 0); seed(value, object);
            if (mode == 1 || mode == 2) throw bodyFailure;
            return prefix;
        }
        public boolean choose(Core self, int value) throws Throwable {
            require(events.equals(Arrays.asList("enter")), "choose order");
            events.add("choose"); check(self, 1);
            require(value == selector, "selector changed");
            if (mode == 3 || mode == 4 || mode == 18) throw bodyFailure;
            cleanupStage = 2;
            return selector != 0;
        }
        private Object value(Core self, long value, Object object, boolean left) throws Throwable {
            require(events.equals(Arrays.asList("enter", "choose")), "value order or duplicate");
            require(left == (selector != 0), "branch or receiver changed");
            events.add(left ? "left" : "right"); check(self, 2); seed(value, object);
            require(++valueCount == 1, "duplicate producer");
            if (mode == 5 || mode == 6 || mode == 9) throw bodyFailure;
            cleanupStage = 3;
            return payload;
        }
        public Object left(Core self, long value, Object object) throws Throwable {
            return value(self, value, object, true);
        }
        public Object right(Core self, long value, Object object) throws Throwable {
            return value(self, value, object, false);
        }
        public void cleanup(Core self, Object object) throws Throwable {
            require(++cleanupCount == 1, "cleanup repeated");
            require(object == token, "cleanup token changed");
            events.add("cleanup"); check(self, cleanupStage);
            token[0] = replacement;
            if (mode == 7 || mode == 8 || mode == 9 || mode == 18) throw cleanupFailure;
        }
        public long finish(Core self, long value) throws Throwable {
            events.add("finish"); check(self, 3);
            require(value == seed && cleanupCount == 1 && token[0] == replacement, "finish order or wide seed");
            if (mode == 10 || mode == 11) throw bodyFailure;
            return tail;
        }
        public Object marker(Core self, Object object) throws Throwable {
            events.add("marker"); check(self, 4);
            require(object == token, "marker token changed");
            if (mode == 12 || mode == 13) throw bodyFailure;
            return marker;
        }
        Throwable expectedFailure() {
            if (mode == 7 || mode == 8 || mode == 9 || mode == 18) return cleanupFailure;
            return mode >= 1 && mode <= 13 ? bodyFailure : null;
        }
        int expectedStage() {
            if (mode == 1 || mode == 2 || mode == 16) return 0;
            if (mode == 3 || mode == 4 || mode == 18) return 1;
            if (mode == 5 || mode == 6 || mode == 9) return 2;
            if (mode == 7 || mode == 8 || mode == 10 || mode == 11) return 3;
            if (mode == 12 || mode == 13) return 4;
            return 5;
        }
        List<String> expectedEvents() {
            List<String> result = new ArrayList<String>();
            if (mode == 16) return result;
            result.add("enter");
            if (mode == 1 || mode == 2) return result;
            result.add("choose");
            if (mode != 3 && mode != 4 && mode != 18) result.add(selector != 0 ? "left" : "right");
            result.add("cleanup");
            if ((mode >= 3 && mode <= 9) || mode == 18) return result;
            result.add("finish");
            if (mode == 10 || mode == 11) return result;
            result.add("marker");
            return result;
        }
    }

    private static Object compiledCaller(Context context, long seed, int selector, Object token) throws Throwable {
        return new Core(context, seed, selector, token);
    }
    private static Object construct(int route, Observer observer) throws Throwable {
        Context context = observer.mode == 16 ? null : observer;
        if (route == 0) {
            try {
                return Core.class.getConstructor(Context.class, long.class, int.class, Object.class)
                        .newInstance(context, observer.seed, observer.selector, observer.token);
            } catch (InvocationTargetException failure) {
                throw failure.getCause();
            }
        }
        if (route == 1) {
            MethodHandle call = MethodHandles.publicLookup().findConstructor(Core.class,
                    MethodType.methodType(void.class, Context.class, long.class, int.class, Object.class))
                    .asType(MethodType.methodType(Object.class, Context.class, long.class, int.class, Object.class));
            return (Object)call.invokeExact(context, observer.seed, observer.selector, (Object)observer.token);
        }
        return compiledCaller(context, observer.seed, observer.selector, observer.token);
    }

    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]);
        int branch = Integer.parseInt(args[1]);
        int mode = Integer.parseInt(args[2]);
        for (int attempt = 0; attempt < 2; ++attempt) {
            long seed = attempt == 0 ? Long.MIN_VALUE + 0x12345678L : Long.MAX_VALUE - 0x76543210L;
            Observer observer = new Observer(mode, mode == 19 ? branch ^ attempt : branch, seed);
            Trace.leaked = null;
            Object result = null;
            Throwable failure = null;
            try { result = construct(route, observer); } catch (Throwable caught) { failure = caught; }
            if (mode == 16) require(failure != null && failure.getClass() == NullPointerException.class, "null context exception");
            else require(failure == observer.expectedFailure(), "exception identity or replacement");
            require(Trace.leaked != null, "constructor disappeared");
            observer.check(Trace.leaked, observer.expectedStage());
            require(result == (failure == null ? Trace.leaked : null), "constructor result identity");
            require(observer.events.equals(observer.expectedEvents()), "event order " + observer.events);
            require(observer.cleanupCount == (mode == 1 || mode == 2 || mode == 16 ? 0 : 1), "cleanup coverage");
            if (observer.cleanupCount != 0) require(observer.token[0] == observer.replacement, "cleanup effect lost");
        }
        System.out.println("attempts=2,ordered-final-stages=matched");
    }
}
