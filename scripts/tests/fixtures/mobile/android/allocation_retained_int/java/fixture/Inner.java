package fixture;
public final class Inner {
    static { Trace.hit(2); }
    public final int first,second;
    public Inner(int first,int second) {
        Trace.partial=this; Trace.hit(6);
        this.first=first; this.second=second; Trace.hit(7);
    }
}
