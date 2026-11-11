import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class BoundedFormalSuperCallHarness extends Leaf {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned bounded formal super call");
    }
    public static final class Future extends Child<Leaf> {
        public boolean run(Context context, Leaf value, int bits) {
            Trace.hit(7); return super.run(context, value, bits);
        }
    }
    @SuppressWarnings("unchecked")
    static <E extends Throwable> void raise(Throwable failure) throws E { throw (E)failure; }
    @SuppressWarnings({"rawtypes", "unchecked"})
    static boolean call(int route, Base receiver, Context context, Bound value, int bits) throws Throwable {
        if (route == 0) return receiver.run(context, value, bits);
        if (route == 1) return BoundedFormalSuperCallCaller.call(receiver, context, value, bits);
        if (route == 2) {
            try { return (Boolean)Base.class.getMethod("run", Context.class, Bound.class, int.class)
                .invoke(receiver, context, value, bits); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        MethodHandle handle = MethodHandles.publicLookup().findVirtual(Base.class, "run",
            MethodType.methodType(boolean.class, Context.class, Bound.class, int.class));
        return (boolean)handle.invokeExact(receiver, context, value, bits);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), future = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        final Base<Leaf> receiver = future == 0 ? Leaf.create() : new Future();
        final Context context = new Context();
        final Bound value = mode == 9 ? null : mode == 10 ? new Bound() : new Leaf();
        final Object replacement = new Object();
        final int bits = mode == 0 ? Integer.MIN_VALUE : mode == 1 ? Integer.MAX_VALUE : mode * 37;
        final boolean result = mode % 2 == 0;
        final RuntimeException runtimeFailure = new RuntimeException("owned callback failure");
        final Error errorFailure = new Error("owned callback error");
        final Exception checkedFailure = new Exception("owned callback checked failure");
        Trace.failure = new RuntimeException("owned stage failure");
        Trace.failAt = mode == 1 ? 1 : mode == 2 ? 2 : mode >= 6 && mode <= 8 ? mode - 2 : 0;
        Trace.action = new Runnable() { public void run() {
            require(Trace.events == (future == 0 ? 12 : 712));
            require(Trace.receiver == receiver && Trace.context == context && Trace.value == value && Trace.bits == bits);
            Trace.hit(3); Trace.result = result;
            if (mode == 3) throw runtimeFailure;
            if (mode == 4) throw errorFailure;
            if (mode == 5) BoundedFormalSuperCallHarness.<RuntimeException>raise(checkedFailure);
            if (mode == 11) { Trace.context = replacement; Trace.value = replacement; Trace.bits = ~bits; value.mark = 99; }
        }};
        for (int attempt = 0; attempt < 2; ++attempt) {
            Object sentinel = new Object();
            Trace.events = 0; Trace.receiver = sentinel; Trace.context = sentinel;
            Trace.value = sentinel; Trace.after = sentinel; Trace.bits = 0; Trace.result = !result;
            if (value != null) value.mark = 0;
            Throwable failure = null; boolean actual = !result;
            try { actual = call(route, receiver, context, value, bits); } catch (Throwable error) { failure = error; }
            String tail = mode == 1 ? "1" : mode == 2 ? "12" : mode >= 3 && mode <= 5 ? "123"
                : mode == 6 ? "1234" : mode == 7 ? "12345" : "123456";
            long trace = mode == 10 ? 0 : Long.parseLong((future == 0 ? "" : "7") + tail);
            require(Trace.events == trace);
            boolean published = mode != 1 && mode != 10;
            require(Trace.receiver == (published ? receiver : sentinel));
            require(Trace.context == (published ? mode == 11 ? replacement : context : sentinel));
            require(Trace.value == (published ? mode == 11 ? replacement : value : sentinel));
            require(Trace.bits == (published ? mode == 11 ? ~bits : bits : 0));
            require(Trace.result == (mode == 1 || mode == 2 || mode == 10 ? !result : result));
            require(Trace.after == (mode == 0 || mode == 8 || mode == 9 || mode == 11 ? value : sentinel));
            if (value != null) require(value.mark == (mode == 11 ? 99 : 0));
            Throwable wanted = mode == 1 || mode == 2 || mode >= 6 && mode <= 8 ? Trace.failure
                : mode == 3 ? runtimeFailure : mode == 4 ? errorFailure : mode == 5 ? checkedFailure : null;
            if (mode == 10) require(failure instanceof ClassCastException);
            else require(failure == wanted);
            require(actual == (wanted == null && mode != 10 ? result : !result));
            System.out.println("route=" + route + ",future=" + future + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
