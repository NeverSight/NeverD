import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class CapturedRawSuperHarness {
    static int mode;
    static Object output;
    static Throwable failure;
    static void check(boolean v) { if (!v) throw new AssertionError("owned captured raw super"); }
    static final class FutureCallback implements Callback {
        int calls; Base seen; Owner enclosing;
        public Object call(Base value) {
            calls++; seen = value;
            try {
                Field capture = value.getClass().getDeclaredField("this$0"); capture.setAccessible(true);
                enclosing = (Owner) capture.get(value);
            } catch (Exception error) { throw new AssertionError(error); }
            check(enclosing == Owner.current);
            if (calls == 1) check(enclosing.stage == 1 && enclosing.child == null && Owner.escaped == value);
            if (mode == 2) throw (RuntimeException) failure;
            if (mode == 6) throw (Error) failure;
            if (mode == 8 || mode == 9) enclosing.stage = 99;
            return output;
        }
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]); mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; attempt++) {
            Owner.events = 0; Owner.current = null; Owner.escaped = null;
            failure = mode == 6 ? new AssertionError("owned callback error") : new RuntimeException("owned callback");
            RuntimeException providerFailure = new RuntimeException("owned parent");
            Owner.failure = mode == 3 || mode == 9 ? providerFailure : null;
            FutureCallback callback = new FutureCallback(); Callback supplied = mode == 1 ? null : callback;
            output = mode == 4 ? null : mode == 5 ? new Long(Long.MIN_VALUE + attempt) : mode == 10 ? new Long(Long.MAX_VALUE - attempt) : new String("value" + attempt);
            Owner owner = null; Throwable caught = null;
            try {
                if (route == 0) owner = new Owner(supplied);
                else if (route == 1) owner = Owner.class.getConstructor(Callback.class).newInstance(supplied);
                else if (route == 2) owner = (Owner) MethodHandles.lookup().findConstructor(Owner.class, MethodType.methodType(void.class, Callback.class)).invokeWithArguments(supplied);
                else owner = Owner.create(supplied);
            } catch (Throwable error) { caught = error instanceof InvocationTargetException ? error.getCause() : error; }
            boolean success = mode != 1 && mode != 2 && mode != 3 && mode != 6 && mode != 9;
            if (success) check(caught == null && owner == Owner.current);
            else if (mode == 1) check(caught instanceof NullPointerException);
            else check(caught == (mode == 3 || mode == 9 ? providerFailure : failure));
            int events = success ? 2079 : 16;
            check(Owner.events == events && callback.calls == (mode == 1 ? 0 : 1));
            Owner captured = Owner.current; Base parent = Owner.escaped;
            check(captured != null && parent != null && parent.saved == supplied);
            check(captured.stage == (success ? 2 : mode == 9 ? 99 : 1));
            check(parent.value == (mode == 1 || mode == 2 || mode == 6 ? null : output));
            check(parent.marker == (mode == 1 || mode == 2 || mode == 6 ? null : captured));
            check(captured.child == (success ? parent : null));
            if (mode != 1) check(callback.enclosing == captured && callback.seen == parent);
            if (success) {
                check(owner.child.owner() == owner && parent.get() == output);
                Base<String> later = (Base<String>) parent; String value = null; Throwable late = null;
                try { value = later.get(); } catch (Throwable error) { late = error; }
                check(output == null || output instanceof String ? late == null && value == output : late instanceof ClassCastException);
                Object future = new String("future"); output = future;
                check(parent.saved.call(parent) == future && callback.calls == 2 && parent.value != future && Owner.events == events);
            }
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + events);
        }
    }
}
