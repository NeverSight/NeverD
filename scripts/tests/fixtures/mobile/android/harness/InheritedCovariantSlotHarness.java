import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class InheritedCovariantSlotHarness {
    static void check(boolean condition) {
        if (!condition) throw new AssertionError("inherited covariance contract");
    }
    static Method bridge(Class<?> owner, Class<?> result) {
        for (Method method : owner.getDeclaredMethods()) {
            if (!method.getName().equals("update") || method.getReturnType() != result) continue;
            check(method.isBridge() && method.isSynthetic());
            Class<?>[] exceptions = method.getExceptionTypes();
            check(result == Root.class ? exceptions.length == 1 && exceptions[0] == Failure.class
                                       : exceptions.length == 0);
            return method;
        }
        throw new AssertionError("missing original inherited covariance slot");
    }
    static Object reflected(Method method, Worker receiver, int code, Object[] values,
                            long wide, double real) throws Throwable {
        try { return method.invoke(receiver, code, values, wide, real); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    static Object call(int route, Worker receiver, int code, Object[] values,
                       long wide, double real) throws Throwable {
        switch (route) {
        case 0: return receiver.update(code, values, wide, real);
        case 1: return ((Base) receiver).update(code, values, wide, real);
        case 2: return ((Editable) receiver).update(code, values, wide, real);
        case 3: return ((Root) receiver).update(code, values, wide, real);
        case 4: return reflected(bridge(Worker.class, Root.class), receiver, code, values, wide, real);
        case 5: return reflected(bridge(Worker.class, Editable.class), receiver, code, values, wide, real);
        case 6: return MethodHandles.lookup().findVirtual(Worker.class, "update",
                    MethodType.methodType(Root.class, int.class, Object[].class, long.class, double.class))
                    .invoke(receiver, code, values, wide, real);
        case 7: return MethodHandles.lookup().findVirtual(Worker.class, "update",
                    MethodType.methodType(Editable.class, int.class, Object[].class, long.class, double.class))
                    .invoke(receiver, code, values, wide, real);
        case 8: return reflected(bridge(Base.class, Root.class), receiver, code, values, wide, real);
        default: return reflected(bridge(Base.class, Editable.class), receiver, code, values, wide, real);
        }
    }
    public static void main(String[] args) throws Throwable {
        for (Class<?> owner : new Class<?>[]{Base.class, Worker.class}) {
            bridge(owner, Root.class);
            bridge(owner, Editable.class);
            check(owner.getDeclaredMethod("update", int.class, Object[].class, long.class, double.class)
                       .getReturnType() == Base.class);
        }
        for (Method method : InheritedCovariantSlotDerived.class.getDeclaredMethods()) {
            check(!method.isBridge());
        }
        int calls = 0, failures = 0;
        for (int derived = 0; derived < 2; derived++) {
            for (int route = 0; route < 10; route++) {
                for (int mode = 0; mode < 8; mode++) {
                    Worker target = derived == 0 ? new Worker() : new InheritedCovariantSlotDerived();
                    Base replacement = mode == 1 ? null : mode == 6 ? target : new Base();
                    target.replacement = replacement;
                    Object seen = new Object();
                    Object[] values = mode == 2 ? null : mode == 3 ? new Object[0] : new Object[]{seen};
                    RuntimeException failure = new IllegalStateException("owned-failure");
                    Trace.failure = mode == 4 ? failure : null;
                    Trace.steps = Trace.derivedCalls = 0;
                    int code = 0x73620000 + mode;
                    long wide = 0xfedcba9876543210L ^ route;
                    double real = Double.longBitsToDouble(mode == 7 ? 0x7ff8000000000015L : 0x8000000000000000L);
                    Object result = null;
                    Throwable caught = null;
                    try { result = call(route, mode == 5 ? null : target, code, values, wide, real); }
                    catch (Throwable error) { caught = error; }
                    calls++;
                    if (mode >= 2 && mode <= 5) {
                        failures++;
                        if (mode == 4) check(caught == failure);
                        else check(caught != null && caught.getClass() ==
                                   (mode == 3 ? ArrayIndexOutOfBoundsException.class : NullPointerException.class));
                    } else check(caught == null && result == replacement);
                    check(Trace.derivedCalls == (mode == 5 ? 0 : derived));
                    check(Trace.steps == (mode == 5 ? 0 : mode >= 2 && mode <= 4 ? 12 : 1234));
                    check(target.calls == (mode == 5 ? 0 : 1));
                    check(target.scalar() == target.calls);
                    if (mode != 5) {
                        check(target.code == code && target.array == values && target.wide == wide);
                        check(Double.doubleToRawLongBits(target.real) == Double.doubleToRawLongBits(real));
                        check(target.seen == (mode == 2 || mode == 3 ? null : seen));
                    }
                }
            }
        }
        System.out.println("calls=" + calls + ",failures=" + failures + ",original-slots-throws-and-dispatch=matched");
    }
}
