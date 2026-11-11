package fixture;

// The runner records the two JVM-only changes: generic class Signature and
// the void target's shared name. All original Code attributes are retained.
@SuppressWarnings("rawtypes")
public final class Worker implements Action {
    public int calls;
    public RuntimeException failure;
    public void run() {
        Trace.events = Trace.events * 10 + 1;
        ++calls;
        if (failure != null) throw failure;
    }
    public Object invoke() {
        run();
        return kotlin.Unit.INSTANCE;
    }
    public static void call(Worker worker) { worker.run(); }
    public void __neverdVoidBridgeTarget() {}
}
