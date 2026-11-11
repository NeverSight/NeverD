package fixture;
public abstract class AbstractWidget extends Root {
    public abstract Object item();
    protected abstract void touch(Object marker);
    public <M> M identity(M value) { touches++; return value; }
}
