package fixture;

public final class OtherItem implements Item {
    public final Value delegate;
    public OtherItem(Value delegate) { this.delegate = delegate; }
    public Object get() { return delegate.get(); }
}
