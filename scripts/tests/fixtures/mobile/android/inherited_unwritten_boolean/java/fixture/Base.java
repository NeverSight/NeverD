package fixture;
public abstract class Base {
    public final long baseValue;
    protected Base() {
        callback();
        this.baseValue=Trace.input+1;
        Trace.observe((Owner)this,2);
    }
    protected abstract void callback();
}
