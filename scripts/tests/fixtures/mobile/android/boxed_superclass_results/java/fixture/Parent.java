package fixture;
public abstract class Parent<T> extends Base<T> implements Api<T> {
    protected Parent() {}
    public abstract T get(int index);
    public <R> R echo(R value) { return value; }
}
