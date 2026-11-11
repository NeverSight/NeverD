import fixture.*;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;

public final class GuardedPrefixZeroHarness {
    static void check(boolean ok) {
        if (!ok) throw new AssertionError("owned guarded prefix zero mismatch");
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[2]);
        boolean alternate = Integer.parseInt(args[1]) != 0;
        Constructor<Core> constructor = Core.class.getConstructor(Object.class, boolean.class);
        MethodHandle handle = MethodHandles.lookup().unreflectConstructor(constructor);
        for (int attempt = 0; attempt < 2; ++attempt) {
            Observer observer = new Observer(alternate, mode);
            Trace.hook = observer;
            Object input = observer.input;
            Core result = null;
            Throwable caught = null;
            try {
                if (route == 0) result = new Core(input, alternate);
                else if (route == 1) {
                    try { result = constructor.newInstance(input, alternate); }
                    catch (InvocationTargetException failure) { throw failure.getCause(); }
                } else result = (Core)handle.invokeExact(input, alternate);
            } catch (Throwable failure) { caught = failure; }
            check(caught == observer.expectedFailure());
            check(observer.events.equals(observer.expectedEvents()));
            check((observer.escaped == null) == (mode == 4));
            if (observer.escaped != null) {
                check(observer.escaped.seed == observer.seed && observer.escaped.token == null);
            }
            if (caught == null) check(result == observer.escaped);
        }
        System.out.println("attempts=2,guarded-prefix-zero=matched");
    }
    public static final class Observer extends Trace {
        final int seed, mode;
        final Object input;
        Object changedInput;
        final Exception checked = new Exception("owned checked failure");
        final Error error = new LinkageError("owned error failure");
        Core escaped;
        String events = "";
        Observer(boolean alternate, int mode) {
            this.mode = mode; seed = alternate ? 17 : 29;
            input = mode == 1 ? new Base() : mode == 2 ? new Object()
                : mode == 3 ? null : mode == 8 ? new Token() : new Child();
            changedInput = input;
        }
        @Override public void observe(int phase, Object self, Object value, int incomingSeed) {
            events += phase + ",";
            if (phase == 0) {
                check(self == null && escaped == null && value == null && incomingSeed == seed);
                if (mode == 9) changedInput = new Object();
            } else {
                check(self instanceof Core && ((Parent)self).seed == seed
                    && ((Parent)self).token == null);
                if (phase == 1) {
                    check(escaped == null && value == null && incomingSeed == seed);
                    escaped = (Core)self;
                } else {
                    check(self == escaped && incomingSeed == 0);
                    if (phase == 2) check(value == input && value instanceof Child);
                    else check(phase == 3 && value == null);
                }
            }
            if ((phase == 0 && mode == 4) || (phase == 2 && mode == 6)) fail(checked);
            if ((phase == 1 && mode == 5) || (phase == 3 && mode == 7)) fail(error);
        }
        Throwable expectedFailure() {
            return mode == 4 || mode == 6 ? checked : mode == 5 || mode == 7 ? error : null;
        }
        String expectedEvents() {
            if (mode == 4) return "0,";
            if (mode == 5) return "0,1,";
            String result = "0,1,";
            if (input instanceof Child) result += "2,";
            return mode == 6 ? result : result + "3,";
        }
        @SuppressWarnings("unchecked")
        static <T extends Throwable> void fail(Throwable failure) throws T { throw (T)failure; }
    }
}
