package fixture;
public abstract class Base<S extends Broad<?>> {
    public int visits;
    protected abstract S[] allocate(int length);
    public S[] dispatch(int length) { visits++; return allocate(length); }
}
