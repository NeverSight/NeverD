package fixture;
public abstract class Base {
    public Base() {}
    public abstract void dispatch(Object first, Object second);
    public <T> T echo(T value) { return value; }
}
