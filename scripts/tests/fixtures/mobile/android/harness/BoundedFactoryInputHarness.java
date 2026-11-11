import fixture.*;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

public final class BoundedFactoryInputHarness {
    static void check(boolean value) {
        if (!value) throw new AssertionError("owned bounded factory input mismatch");
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    static Factory<Leaf> direct(Producer producer, Config config, long bits) {
        return Core.call(producer, config, bits);
    }

    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Method reflected = Core.class.getMethod("call", Producer.class, Config.class, long.class);
        MethodHandle handle = MethodHandles.lookup().findStatic(Core.class, "call",
            MethodType.methodType(Factory.class, Producer.class, Config.class, long.class));
        for (int attempt = 0; attempt < 2; ++attempt) {
            Observer observer = new Observer(mode);
            Trace.hook = observer;
            Factory<?> result = null;
            Leaf value = null;
            Throwable caught = null;
            try {
                if (route == 0) {
                    try { result = (Factory<?>)reflected.invoke(null, observer.callback,
                        observer.config, observer.bits); }
                    catch (InvocationTargetException failure) { throw failure.getCause(); }
                } else if (route == 1) {
                    result = (Factory<?>)handle.invokeExact((Producer)observer.callback,
                        observer.config, observer.bits);
                } else result = direct(observer.callback, observer.config, observer.bits);
                check(result == observer.expectedFactory());
                if (result != null) value = ((Factory<Leaf>)result).get();
            } catch (Throwable failure) { caught = failure; }
            if (mode == 2 || mode == 6) check(caught instanceof ClassCastException);
            else if (mode == 3) check(caught instanceof NullPointerException);
            else check(caught == observer.expectedFailure());
            check(observer.events.equals(observer.expectedEvents()));
            if (caught == null) check(value == (result == null ? null : observer.leaf));
            check(observer.producerCalls == (mode == 3 || mode == 9 || mode == 10 ? 0 : 1));
        }
        System.out.println("attempts=2,bounded-factory-input=matched");
    }

    public static final class Child extends Base {
        Child(Object tag) { super(tag); }
    }

    static final class Dual implements Producer<Object, Object>, Factory<Object> {
        final Observer owner;
        Dual(Observer owner) { this.owner = owner; }
        @Override public Object produce(Object input) {
            owner.events += "P,";
            ++owner.producerCalls;
            check(input == (owner.config == null ? null : owner.config.input));
            if (owner.mode == 7) Observer.fail(owner.checked);
            if (owner.mode == 8) Observer.fail(owner.error);
            return owner.produced;
        }
        @Override public Object get() { return owner.read(); }
    }

    public static final class Observer extends Trace {
        final int mode;
        final Object shared = new Object();
        final Context initial = new Context(shared), replacement = new Context(new Object());
        final Config config;
        final long bits;
        final Object produced;
        final Leaf leaf;
        final Dual dual;
        final Producer<?, ?> callback;
        final Factory<?> first;
        final Exception checked = new Exception("owned checked");
        final Error error = new LinkageError("owned error");
        Factory<?> current;
        int producerCalls;
        String events = "";

        Observer(int mode) {
            this.mode = mode;
            config = mode == 16 ? null : new Config(initial);
            bits = mode == 19 ? Long.MIN_VALUE : mode == 20 ? 0x0123456789abcdefL
                : mode == 21 ? 0L : 0xff00ff00abcdef12L;
            produced = mode == 1 ? null : mode == 2 ? "owned wrong producer result"
                : new Child(shared);
            leaf = mode == 18 ? null : new Leaf(shared);
            dual = new Dual(this);
            callback = mode == 3 ? null : dual;
            first = mode == 4 ? null : mode == 5 ? dual : new Factory<Object>() {
                @Override public Object get() { return read(); }
            };
            current = first;
        }

        @Override public void observe(int phase, Object producer, Config context,
                                      long valueBits, Object value) {
            events += phase + ",";
            check(context == config);
            if (phase == 6) {
                check(producer == null && valueBits == 0L && value == null);
                if (mode == 22) config.input = replacement;
                return;
            }
            check(producer == callback && valueBits == bits);
            if (phase == 1 || phase == 2) check(value == null);
            else if (phase == 3) check(value == produced);
            else if (phase == 4 || phase == 5) check(value == first);
            else check(false);
            if (phase == 1 && mode == 9) fail(checked);
            if (phase == 2 && mode == 10) fail(error);
            if (phase == 3 && mode == 11) fail(checked);
            if (phase == 4 && mode == 12) fail(error);
            if (phase == 5 && mode == 13) fail(checked);
            if (phase == 4 && mode == 23) current = new Factory<Object>() {
                @Override public Object get() {
                    throw new AssertionError("owned bounded factory input mismatch");
                }
            };
        }

        @Override @SuppressWarnings({"rawtypes", "unchecked"})
        public Factory<Leaf> provide(Base value, Config context, long valueBits) {
            events += "F,";
            check(value == produced && context == config && valueBits == bits);
            if (mode == 14) fail(checked);
            if (mode == 15) fail(error);
            return (Factory)current;
        }

        Object read() {
            events += "G,";
            return mode == 6 ? "owned wrong factory value" : leaf;
        }
        Factory<?> expectedFactory() { return first; }

        Throwable expectedFailure() {
            if (mode == 7 || mode == 9 || mode == 11 || mode == 13 || mode == 14) return checked;
            if (mode == 8 || mode == 10 || mode == 12 || mode == 15) return error;
            return null;
        }

        String expectedEvents() {
            if (mode == 9) return "1,";
            if (mode == 10) return "1,2,";
            if (mode == 3) return "1,2,6,";
            String prefix = "1,2,6,P,";
            if (mode == 2 || mode == 7 || mode == 8) return prefix;
            prefix += "3,";
            if (mode == 11) return prefix;
            prefix += "F,";
            if (mode == 14 || mode == 15) return prefix;
            prefix += "4,";
            if (mode == 12) return prefix;
            prefix += "5,";
            if (mode == 13 || mode == 4) return prefix;
            return prefix + "G,";
        }

        @SuppressWarnings("unchecked")
        static <T extends Throwable> void fail(Throwable failure) throws T {
            throw (T)failure;
        }
    }
}
