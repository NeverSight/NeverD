import fixture.*;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

public final class NominalResultOnlyFactoryHarness {
    static void check(boolean ok) {
        if (!ok) throw new AssertionError("owned result only factory mismatch");
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Method reflected = Core.class.getMethod("call", Context.class);
        MethodHandle handle = MethodHandles.lookup().findStatic(Core.class, "call",
            MethodType.methodType(Holder.class, Context.class));
        for (int attempt = 0; attempt < 2; ++attempt) {
            Observer observer = new Observer(mode);
            Trace.hook = observer;
            Throwable caught = null;
            Child value = null;
            try {
                Holder<?> result;
                if (route == 0) {
                    try { result = (Holder<?>)reflected.invoke(null, observer.context); }
                    catch (InvocationTargetException failure) { throw failure.getCause(); }
                } else if (route == 1) result = (Holder<?>)handle.invokeExact(observer.context);
                else result = Core.call(observer.context);
                check(result == observer.wanted);
                value = (Child)result.get();
            } catch (Throwable failure) { caught = failure; }
            if (mode == 2 || mode == 15) check(caught instanceof ClassCastException);
            else if (mode == 4) check(caught instanceof NullPointerException);
            else check(caught == observer.expectedFailure());
            check(observer.events.equals(observer.expectedEvents()));
            if (caught == null) check(value == observer.expectedValue);
        }
        System.out.println("attempts=2,result-only-factory=matched");
    }
    public static final class Child extends Base {
        Child(Object tag) { super(tag); }
    }
    static final class TrapHolder extends Holder<Base> {
        final Observer observer;
        TrapHolder(Observer observer, Base value) { super(value); this.observer = observer; }
        @Override public Base get() {
            observer.events += "G,";
            if (observer.mode == 12) Observer.fail(observer.checked);
            return value;
        }
    }
    public static final class Observer extends Trace {
        final int mode;
        final Child firstValue = new Child(new Object()), secondValue = new Child(new Object());
        final Holder<Base> first, second;
        final Context context;
        Holder<?> wanted;
        Child expectedValue;
        final Exception checked = new Exception("owned checked failure");
        final Error error = new LinkageError("owned error failure");
        final RuntimeException missing = new IllegalStateException("owned missing identity");
        String events = "";

        Observer(int mode) {
            this.mode = mode;
            first = new TrapHolder(this, mode == 1 ? null : mode == 15 ? new Base(new Object()) : firstValue);
            second = new TrapHolder(this, secondValue);
            wanted = first;
            expectedValue = mode == 1 ? null : firstValue;
            Object offered = mode == 2 ? "owned wrong holder" :
                mode == 3 || mode == 4 || mode == 13 || mode == 14 ? null : first;
            context = mode == 5 ? null : new Context(offered);
        }

        @Override public void observe(int phase, Context input, Object result) {
            events += phase + ",";
            check(input == context);
            if (phase == 1 || phase == 2) check(result == null);
            else check(result == wanted);
            if (phase == 1 && mode == 6) fail(checked);
            if (phase == 2 && mode == 7) fail(error);
            if (phase == 3 && mode == 10) fail(checked);
            if (phase == 4 && mode == 11) fail(error);
            if (phase == 2 && mode == 17) {
                context.offered = second;
                wanted = second;
                expectedValue = secondValue;
            }
            if (phase == 3 && mode == 16) context.offered = second;
        }
        @Override public Object provide(Context input) {
            events += "F,";
            check(input == context);
            if (mode == 8) fail(checked);
            if (mode == 9) fail(error);
            return context == null ? first : context.offered;
        }
        @Override public RuntimeException onMissing(Context input) {
            events += "M,";
            check(input == context);
            if (mode == 13) fail(checked);
            if (mode == 14) fail(error);
            return mode == 4 ? null : missing;
        }
        Throwable expectedFailure() {
            if (mode == 3) return missing;
            if (mode == 6 || mode == 8 || mode == 10 || mode == 12 || mode == 13) return checked;
            if (mode == 7 || mode == 9 || mode == 11 || mode == 14) return error;
            return null;
        }
        String expectedEvents() {
            if (mode == 6) return "1,";
            if (mode == 7) return "1,2,";
            if (mode == 2 || mode == 8 || mode == 9) return "1,2,F,";
            if (mode == 3 || mode == 4 || mode == 13 || mode == 14) return "1,2,F,M,";
            if (mode == 10) return "1,2,F,3,";
            if (mode == 11) return "1,2,F,3,4,";
            return "1,2,F,3,4,G,";
        }
        @SuppressWarnings("unchecked")
        static <T extends Throwable> void fail(Throwable failure) throws T {
            throw (T)failure;
        }
    }
}
