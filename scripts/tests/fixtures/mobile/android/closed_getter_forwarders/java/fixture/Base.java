package fixture;
public abstract class Base {
    public Base() {}
    public abstract Box<Throwable> items();
    public <T> T echo(T value) { return value; }
}
