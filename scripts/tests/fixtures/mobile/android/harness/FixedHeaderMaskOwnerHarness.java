import fixture.*;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import kotlin.jvm.internal.DefaultConstructorMarker;

public final class FixedHeaderMaskOwnerHarness {
    static void check(boolean ok) {
        if (!ok) throw new AssertionError("owned fixed header mask mismatch");
    }
    public static final class Child extends Base { Child(Object tag) { super(tag); } }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), branch = Integer.parseInt(args[1]);
        int mode = Integer.parseInt(args[2]);
        Constructor<?> mask = Core.class.getDeclaredConstructor(Base.class, Object.class,
            int.class, DefaultConstructorMarker.class);
        mask.setAccessible(true);
        MethodHandle handle = MethodHandles.lookup().unreflectConstructor(mask);
        Constructor<DefaultConstructorMarker> markerConstructor =
            DefaultConstructorMarker.class.getDeclaredConstructor();
        markerConstructor.setAccessible(true);
        DefaultConstructorMarker marker = mode >= 10 ? markerConstructor.newInstance() : null;
        for (int attempt = 0; attempt < 2; ++attempt) {
            Observer observer = new Observer(route, branch, mode);
            Trace.hook = observer;
            Core<?> result = null;
            Throwable caught = null;
            int bits = branch == 1 ? 2 : 0;
            if (mode == 9) bits |= Integer.MIN_VALUE;
            if (mode == 10) bits = branch == 1 ? Integer.MAX_VALUE : Integer.MAX_VALUE & ~2;
            try {
                if (route == 0) {
                    try { result = (Core<?>)mask.newInstance(observer.value, observer.inputToken, bits, marker); }
                    catch (InvocationTargetException failure) { throw failure.getCause(); }
                } else if (route == 1) {
                    result = (Core<?>)handle.invokeExact(observer.value, observer.inputToken, bits, marker);
                } else if (mode == 11) {
                    result = new Core<Base>((Producer<Base>)null);
                } else {
                    result = new Core<Base>(new Producer<Base>() {
                        @Override public Base produce() {
                            observer.events += "P,";
                            if (mode == 7) Observer.fail(observer.checked);
                            return observer.value;
                        }
                    });
                }
            } catch (Throwable failure) { caught = failure; }
            if (mode == 1) {
                check(caught != null && caught.getClass() == NullPointerException.class);
                check("null cannot be cast to non-null type T of fixture.Core".equals(caught.getMessage()));
            } else if (route == 2 && mode == 11) {
                check(caught != null && caught.getClass() == NullPointerException.class);
            } else {
                check(caught == observer.expectedFailure());
            }
            check(observer.events.equals(observer.expectedEvents()));
            if (caught == null) {
                check(result == observer.escaped && result.value == observer.value
                    && result.token == observer.expectedToken && result instanceof Marker);
            } else if (observer.escaped != null) {
                check(observer.escaped.value == (mode == 5 || mode == 8 ? null : observer.value));
                check(observer.escaped.token == (mode == 9 ? observer.expectedToken : null));
            }
        }
        System.out.println("attempts=2,fixed-header-mask=matched");
    }
    public static final class Observer extends Trace {
        final int route, mode;
        final Base value;
        final Object inputToken, expectedToken;
        final Exception checked = new Exception("owned checked failure");
        final Error error = new LinkageError("owned error failure");
        Core<?> escaped;
        String events = "";
        Observer(int route, int branch, int mode) {
            this.route = route; this.mode = mode;
            value = mode == 1 ? null : mode == 2 ? new Base(new Object()) : new Child(new Object());
            inputToken = mode == 3 ? null : mode == 4 ? value : new Object();
            expectedToken = route == 2 || branch == 1 ? null : inputToken;
        }
        @Override public void observe(int phase, Object self, Object incoming) {
            events += phase + ",";
            check(self instanceof Core && self instanceof Marker);
            Core<?> core = (Core<?>)self;
            if (phase == 1) {
                check(escaped == null && core.value == null && core.token == null);
                escaped = core;
            } else {
                check(escaped == core);
                check(core.value == (phase == 2 ? null : value));
                check(core.token == (phase == 4 ? expectedToken : null));
            }
            if (phase == 2) check(incoming == value);
            if (phase == 3) check(incoming == expectedToken);
            if (phase == 2 && mode == 5) fail(checked);
            if (phase == 3 && mode == 6) fail(error);
            if (phase == 1 && mode == 8) fail(error);
            if (phase == 4 && mode == 9) fail(checked);
        }
        Throwable expectedFailure() {
            if (mode == 5 || mode == 9 || (route == 2 && mode == 7)) return checked;
            if (mode == 6 || mode == 8) return error;
            return null;
        }
        String expectedEvents() {
            if (route == 2 && mode == 11) return "";
            String prefix = route == 2 ? "P," : "";
            if (route == 2 && mode == 7) return prefix;
            prefix += "1,";
            if (mode == 8) return prefix;
            prefix += "2,";
            if (mode == 5 || mode == 1) return prefix;
            prefix += "3,";
            if (mode == 6) return prefix;
            return prefix + "4,";
        }
        @SuppressWarnings("unchecked")
        static <T extends Throwable> void fail(Throwable failure) throws T { throw (T)failure; }
    }
}
