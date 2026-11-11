package fixture;
public abstract class Trace {
    public static Trace hook;
    public static Provider create() { return hook.open(); }
    public abstract Provider open();
    public abstract Object read(Provider resource);
    public abstract void close(Provider resource);
}
