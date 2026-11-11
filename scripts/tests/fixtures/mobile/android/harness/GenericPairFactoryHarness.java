import fixture.*;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class GenericPairFactoryHarness {
    static void check(boolean ok) {
        if (!ok) throw new AssertionError("owned generic pair factory mismatch");
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        MethodHandle entry = MethodHandles.lookup().findStatic(Core.class, "call",
            MethodType.methodType(Factory.class, Token.class, Token.class, Context.class));
        for (int attempt = 0; attempt < 2; ++attempt) {
            Observer observer = new Observer(mode);
            Trace.hook = observer;
            Factory result = null;
            Throwable caught = null;
            try {
                if (route == 0) result = Core.call(observer.left, observer.right, observer.context);
                else if (route == 1) result = Core.typed(observer.left, observer.right, observer.context);
                else result = (Factory)entry.invokeExact(observer.left, observer.right, observer.context);
            } catch (Throwable failure) { caught = failure; }
            if (mode == 2 || mode == 3 || mode == 4)
                check(caught != null && caught.getClass() == NullPointerException.class);
            else if (mode == 5)
                check(caught != null && caught.getClass() == ClassCastException.class);
            else check(caught == observer.expectedFailure());
            check(observer.events.equals(observer.expectedEvents()));
            if (caught == null) check(result == observer.expectedResult());
            check(observer.result.left == observer.first && observer.result.right == observer.second
                && observer.result.tag == observer.tag);
        }
        System.out.println("attempts=2,generic-pair-factory=matched");
    }
    public static final class Observer extends Trace {
        final int mode;
        final Object first, second, tag = new Object(), offered;
        final Token left, right;
        final Context context;
        final Factory result, replacement;
        final Exception checked = new Exception("owned checked failure");
        final Error error = new LinkageError("owned error failure");
        int reads;
        String events = "";
        Observer(int mode) {
            this.mode = mode;
            first = mode == 15 ? new Object() : mode == 16 ? null : new Base(new Object());
            second = mode == 18 ? new String("owned raw content")
                : mode == 13 ? first : new Base(new Object());
            Token l = new Token(first);
            left = mode == 2 ? null : l;
            right = mode == 3 ? null : mode == 13 ? l : new Token(second);
            result = new Factory(first, second, tag);
            replacement = new Factory(second, first, new Object());
            offered = mode == 5 ? new Object() : mode == 6 ? null : result;
            context = mode == 4 ? null : new Context(offered);
        }
        @Override public void observe(int phase, Object one, Object two, Object three) {
            if (phase == 4) {
                ++reads;
                events += reads == 1 ? "4L," : "4R,";
                check(one == (reads == 1 ? left : right));
                check(two == (reads == 1 ? first : second) && three == null);
                if (reads == 1 && mode == 8) fail(error);
                if (reads == 2 && mode == 9) fail(checked);
                return;
            }
            events += phase + ",";
            if (phase == 1) check(one == left && two == right && three == context);
            else if (phase == 2) check(one == first && two == second && three == context);
            else if (phase == 5) {
                check(one == context && two == offered && three == null);
                if (mode == 19) context.next = replacement;
                if (mode == 17) fail(checked);
            } else {
                check(phase == 3 && one == expectedResult() && two == first && three == second);
                if (mode == 14) context.next = new Object();
            }
            if ((phase == 1 && mode == 7) || (phase == 5 && mode == 11)) fail(checked);
            if ((phase == 2 && mode == 10) || (phase == 3 && mode == 12)) fail(error);
        }
        Throwable expectedFailure() {
            if (mode == 7 || mode == 9 || mode == 11 || mode == 17) return checked;
            if (mode == 8 || mode == 10 || mode == 12) return error;
            return null;
        }
        Object expectedResult() { return mode == 19 ? replacement : offered; }
        String expectedEvents() {
            String s = "1,";
            if (mode == 2 || mode == 7) return s;
            s += "4L,";
            if (mode == 3 || mode == 8) return s;
            s += "4R,";
            if (mode == 9) return s;
            s += "2,";
            if (mode == 4 || mode == 10) return s;
            s += "5,";
            if (mode == 5 || mode == 11 || mode == 17) return s;
            return s + "3,";
        }
        @SuppressWarnings("unchecked")
        static <T extends Throwable> void fail(Throwable failure) throws T { throw (T)failure; }
    }
}
