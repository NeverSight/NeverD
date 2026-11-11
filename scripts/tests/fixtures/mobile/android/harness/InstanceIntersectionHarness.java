import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class InstanceIntersectionHarness {
    static void check(boolean condition) {
        if (!condition) throw new AssertionError("instance intersection contract");
    }
    static void call(Core core, boolean contract, int route) throws Throwable {
        if (route == 0) {
            if (contract) core.runInterface(); else core.runVirtual();
        } else if (route == 1) InstanceIntersectionCaller.run(core, contract);
        else if (route == 2) {
            try { Core.class.getMethod(contract ? "runInterface" : "runVirtual").invoke(core); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        } else MethodHandles.lookup().findVirtual(Core.class, contract ? "runInterface" : "runVirtual",
                MethodType.methodType(void.class)).invokeExact(core);
    }
    public static void main(String[] args) throws Throwable {
        boolean wrongBound = args.length != 0 && args[0].equals("wrong-bound");
        int calls = 0;
        for (int derived = 0; derived < 2; derived++) for (int contract = 0; contract < 2; contract++)
        for (int route = 0; route < 4; route++) for (int mode = 0; mode < 10; mode++)
        for (int attempt = 0; attempt < 2; attempt++) {
            Core core = new Core();
            Child receiver = derived == 0 ? new Child() : new InstanceIntersectionDerived();
            Value value = mode == 9 ? null : new Value();
            core.value = value; core.receiver = mode == 8 ? null : receiver; core.contract = core.receiver;
            Trace.core = core; Trace.replacement = new Value();
            Trace.events = Trace.firstCalls = Trace.secondCalls = 0;
            Trace.receiver = Trace.argument = null;
            Trace.failAt = mode < 8 ? mode : 0;
            RuntimeException original = new IllegalStateException("owned intersection callback failure");
            Trace.failure = original;
            int[] sequence = derived == 0 ? new int[]{1, 2, 3, 4, 5, 6} : new int[]{1, 7, 2, 3, 4, 5, 6};
            int events = 0, first = 0, second = 0;
            Class<?> expected = null;
            boolean sameFailure = false, entered = false;
            for (int step : sequence) {
                if (step != 1 && mode == 8) { expected = NullPointerException.class; break; }
                if (step == 3 && mode == 9) { expected = NullPointerException.class; break; }
                if (step == 5 && wrongBound) { expected = ClassCastException.class; break; }
                if (step != 1) entered = true;
                events = events * 10 + step;
                if (step == 3) first++;
                if (step == 5) second++;
                if (mode == step) { expected = original.getClass(); sameFailure = true; break; }
            }
            Throwable failure = null;
            try { call(core, contract != 0, route); } catch (Throwable error) { failure = error; }
            if (expected == null) check(failure == null);
            else check(failure != null && failure.getClass() == expected && (!sameFailure || failure == original));
            check(Trace.events == events && Trace.firstCalls == first && Trace.secondCalls == second);
            check(core.value == Trace.replacement);
            check(Trace.receiver == (entered ? receiver : null) && Trace.argument == (entered ? value : null));
            calls++;
        }
        check(calls == 320);
        System.out.println("calls=320,bounds=" + (wrongBound ? "missing" : "complete") + ",snapshots-dispatch-and-failures=matched");
    }
}
