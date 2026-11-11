package fixture;
public final class Token {
    static { Trace.hit(1); }
    public final Object first,second,marker;
    public final int mask;
    public Token(Object first,Object second,int mask,Object marker) {
        Trace.token=this; Trace.hit(2);
        this.first=first;this.second=second;this.mask=mask;this.marker=marker;Trace.hit(3);
    }
}
