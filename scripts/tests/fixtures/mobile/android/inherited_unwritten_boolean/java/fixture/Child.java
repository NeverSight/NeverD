package fixture;
public final class Child extends Owner {
    public final int childValue;
    public Child(long value) {
        super(value);
        this.childValue=7;
        Trace.observe(this,5);
    }
    public Child(long value,int marker) {
        super(value,marker);
        Trace.observe(this,4);
        this.childValue=marker;
        Trace.observe(this,5);
    }
}
