package fixture;
public abstract class Owner extends Base {
    public boolean zeroFlag;
    public final long value;
    protected Owner(long value) {
        super();
        this.value=value;
        Trace.observe(this,3);
    }
    protected Owner(long value,int marker) {
        this(value);
    }
    protected final void callback() { Trace.observe(this,1); }
    public final boolean flag() { return zeroFlag; }
}
