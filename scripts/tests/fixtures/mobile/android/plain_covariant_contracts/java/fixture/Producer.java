package fixture;
public class Producer<P> implements GenericApi<P> {
    public int parentCalls;
    public RuntimeException parentFailure;
    public Producer() {}
    public Result<P> create() {
        parentCalls++;
        if (parentFailure != null) throw parentFailure;
        return null;
    }
}
