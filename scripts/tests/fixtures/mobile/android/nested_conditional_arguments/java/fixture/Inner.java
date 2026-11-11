package fixture;
public final class Inner {
    static { Trace.hit(2); }
    public final Object first,second;
    public Inner(Object first,Object second) {
        Trace.hit(5);this.first=first;this.second=second;Trace.inner=this;Trace.hit(6);
    }
}
