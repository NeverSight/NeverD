package fixture;

public final class OwnedSelf extends Base<OwnedSelf> {
    public OwnedSelf value;
    public int calls;
    public RuntimeException failure;

    protected OwnedSelf self() {
        calls++;
        if (failure != null) throw failure;
        return value;
    }

    public OwnedSelf peek() { return self(); }
}
