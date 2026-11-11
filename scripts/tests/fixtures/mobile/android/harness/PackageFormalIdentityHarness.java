import fixture.*;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;

public final class PackageFormalIdentityHarness {
    static void check(boolean ok) {
        if (!ok) throw new AssertionError("owned package formal identity mismatch");
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        MethodHandle call = MethodHandles.lookup().findStatic(Core.class, "call",
            MethodType.methodType(Object.class, Object.class));
        for (int attempt = 0; attempt < 2; ++attempt) {
            Observer observer = new Observer(mode);
            Trace.hook = observer;
            Trace.next = observer.offered;
            Object result = null;
            Throwable caught = null;
            try {
                if (route == 0) result = Core.call(observer.offered);
                else if (route == 1) result = Core.typed(observer.offered);
                else result = (Object)call.invokeExact((Object)observer.offered);
            } catch (Throwable failure) { caught = failure; }
            if (observer.expectedFailure() != null)
                check(caught == observer.expectedFailure());
            else if (mode == 6)
                check(caught != null && caught.getClass() == NullPointerException.class);
            else if (route == 1 && (mode == 2 || mode == 3 || mode == 4
                                  || mode == 5 || mode == 17))
                check(caught != null && caught.getClass() == ClassCastException.class);
            else check(caught == null);
            check(observer.events.equals(observer.expectedEvents()));
            if (caught == null) check(result == observer.offered);
            if (mode == 15 || mode == 16) check(Trace.next == observer.changed);
        }
        System.out.println("attempts=2,package-formal-identity=matched");
    }
    public static final class Observer extends Trace {
        final int mode;
        final Object offered, changed = new Object();
        final Exception failure = new Exception("owned checked failure");
        final Error error = new LinkageError("owned error failure");
        String events = "";
        Observer(int mode) {
            this.mode = mode;
            Object token = new Object();
            offered = mode == 1 ? new Base(token) {}
                : mode == 2 ? new String("owned value")
                : mode == 3 ? new Object() : mode == 4 ? new Object[]{token}
                : mode == 5 ? new int[]{1,2} : mode == 6 || mode == 7
                    || mode == 9 || mode == 10 ? null
                : mode == 17 ? this : new Base(token);
        }
        @Override public void observe(int phase, Object value) {
            events += phase + ",";
            check(value == offered);
            if (phase == 3 && mode == 15 || phase == 4 && mode == 16)
                Trace.next = changed;
            if (phase == 1 && mode == 7 || phase == 2 && mode == 9
                || phase == 3 && mode == 11 || phase == 4 && mode == 13)
                fail(failure);
            if (phase == 1 && mode == 8 || phase == 2 && mode == 10
                || phase == 3 && mode == 12 || phase == 4 && mode == 14)
                fail(error);
        }
        Throwable expectedFailure() {
            if (mode == 7 || mode == 9 || mode == 11 || mode == 13) return failure;
            if (mode == 8 || mode == 10 || mode == 12 || mode == 14) return error;
            return null;
        }
        String expectedEvents() {
            if (mode == 7 || mode == 8) return "1,";
            if (offered == null) return "1,2,";
            if (mode == 11 || mode == 12) return "1,3,";
            return "1,3,4,";
        }
        @SuppressWarnings("unchecked")
        static <T extends Throwable> void fail(Throwable failure) throws T { throw (T)failure; }
    }
}
