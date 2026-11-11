package fixture;
public class Result implements java.io.Serializable {
    static { Trace.hit(1); }
    public Object original;
    public Caller owner;
    public Result(Object original, Caller owner) {
        Trace.hit(6); Trace.wrapper=this; this.original=original;
        Trace.hit(7); this.owner=owner; Trace.hit(8);
    }
}
