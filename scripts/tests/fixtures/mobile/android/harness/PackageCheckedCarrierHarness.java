import fixture.*;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class PackageCheckedCarrierHarness {
    static void check(boolean ok) {
        if (!ok) throw new AssertionError("owned package checked carrier mismatch");
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        MethodHandle call = MethodHandles.lookup().findStatic(Core.class, "call",
            MethodType.methodType(Object.class, Task.class));
        for (int attempt = 0; attempt < 2; ++attempt) {
            Observer observer = new Observer(mode);
            Trace.hook = observer;
            Object result = null;
            Throwable caught = null;
            try {
                if (route == 0) result = Core.call(observer.task);
                else if (route == 1) result = Core.typed((Task)observer.task);
                else result = (Object)call.invokeExact((Task)observer.task);
            } catch (Throwable failure) { caught = failure; }
            if (mode == 5)
                check(caught != null && caught.getClass() == NullPointerException.class);
            else if (route == 1 && (mode == 3 || mode == 17))
                check(caught != null && caught.getClass() == ClassCastException.class);
            else check(caught == observer.expectedFailure());
            check(observer.events.equals(observer.expectedEvents()));
            if (caught == null) check(result == observer.expectedResult());
            check(observer.calls == (mode == 5 || mode == 10 ? 0
                : mode == 12 ? 1 : observer.retries + 1));
        }
        System.out.println("attempts=2,package-checked-carrier=matched");
    }
    public static final class Observer extends Trace {
        final int mode, retries;
        final Task<Object> task;
        final Object offered, changed = new Object();
        final Failure failure = new Failure();
        final Error error = new LinkageError("owned error failure");
        final Retry[] retryObjects = {new Retry(), new Retry(), new Retry()};
        Object next;
        int calls;
        String events = "";
        Observer(int mode) {
            this.mode = mode;
            retries = mode == 2 || mode == 9 ? 2 : mode == 19 ? 3
                : mode == 1 || mode == 8 || mode == 12 || mode == 13
                    || mode == 14 || mode == 16 || mode == 17 || mode == 18 ? 1 : 0;
            offered = mode == 3 ? new Object() : mode == 4 || mode == 16 ? null
                : new Base(new Object());
            next = offered;
            task = mode == 5 ? null : new Task<Object>() {
                @Override public Object get() throws Retry, Failure {
                    events += "G,";
                    ++calls;
                    if (calls <= retries) throw retryObjects[calls - 1];
                    if (mode == 6 || mode == 8) throw failure;
                    if (mode == 7 || mode == 9) throw error;
                    return next;
                }
            };
        }
        @Override public void observe(int phase, Object input, Object value, int count) {
            events += phase + ",";
            check(input == task && count == (mode == 5 && phase != 1 ? 1 : calls));
            if (phase == 2) {
                check(value == expectedResult());
                if (mode == 15) next = changed;
            } else if (phase == 3) {
                check(value == retryObjects[calls - 1] && calls <= retries);
                if (mode == 17) next = changed;
            } else {
                check((phase == 1 || phase == 4 || phase == 5) && value == null);
                if (phase == 4 && mode == 18) next = changed;
            }
            if ((phase == 1 && mode == 10) || (phase == 2 && mode == 11)
                || (phase == 4 && mode == 13)) fail(failure);
            if ((phase == 3 && mode == 12) || (phase == 5 && mode == 14)) fail(error);
        }
        Object expectedResult() { return mode == 17 ? changed : offered; }
        Throwable expectedFailure() {
            if (mode == 6 || mode == 8 || mode == 10 || mode == 11 || mode == 13) return failure;
            if (mode == 7 || mode == 9 || mode == 12 || mode == 14) return error;
            return null;
        }
        String expectedEvents() {
            String result = "1,";
            if (mode == 10) return result;
            boolean retried = false;
            if (mode != 5) {
                for (int i = 0; i <= retries; ++i) {
                    result += "G,";
                    if (i < retries) {
                        result += "3,";
                        retried = true;
                        if (mode == 12) break;
                    } else if (mode != 6 && mode != 7 && mode != 8 && mode != 9) {
                        result += "2,";
                    }
                }
            }
            if (retried) {
                result += "4,";
                if (mode == 13) return result;
            }
            return result + "5,";
        }
        @SuppressWarnings("unchecked")
        static <T extends Throwable> void fail(Throwable failure) throws T { throw (T)failure; }
    }
}
