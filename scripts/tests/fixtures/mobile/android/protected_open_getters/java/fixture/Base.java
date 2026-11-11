package fixture;
public abstract class Base<S extends Broad<?>> {
    public int visits;
    protected abstract S get();
    public S dispatch() { visits++; return get(); }
}
