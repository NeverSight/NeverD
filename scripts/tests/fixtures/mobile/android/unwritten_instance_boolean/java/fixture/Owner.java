package fixture;
public final class Owner {
    public boolean zeroFlag;
    public final long value;
    public Owner(long value,int count) {
        Trace.observe(this,1);
        this.value=value;Trace.observe(this,2);
        for(int i=0;i<count;i++)Trace.observe(this,3);
        Trace.observe(this,4);
    }
    public boolean flag() { return zeroFlag; }
}
