package fixture;

public abstract class Base<T extends Base<T>> {
    public int baseCalls;
    protected abstract T self();
    public T dispatch() { baseCalls++; return self(); }
}
