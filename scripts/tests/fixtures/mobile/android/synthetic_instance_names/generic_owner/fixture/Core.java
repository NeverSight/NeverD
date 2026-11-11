package fixture;
public class Core<K, V> implements Scope<V> {
    public Runnable task(Runnable action) {
        Trace.hit(1);
        return () -> { try { action.run(); } finally { after(); } };
    }
    protected void after() { Trace.receiver = this; Trace.hit(3); }
}
