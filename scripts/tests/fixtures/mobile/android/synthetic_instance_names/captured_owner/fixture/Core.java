package fixture;
public class Core<K, V> {
    public Runnable task(Runnable action) {
        Trace.hit(1);
        return new Worker().task(action);
    }
    protected void after() { Trace.receiver = this; Trace.hit(3); }
    private class Worker implements Scope<java.util.Map.Entry<K, V>> {
        Worker() {}
        Runnable task(Runnable action) {
            return () -> { try { action.run(); } finally { Core.this.after(); } };
        }
    }
}
