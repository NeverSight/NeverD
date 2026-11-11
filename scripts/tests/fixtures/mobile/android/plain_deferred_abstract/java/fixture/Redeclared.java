package fixture;
public abstract class Redeclared extends Deferred implements Api {
    public Redeclared(Sink sink) { super(sink); }
    protected abstract void touch(Object marker);
    public abstract Object item();
    public abstract <Q> Q identity(Q value);
}
