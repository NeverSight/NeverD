package fixture;
public class Base {
    public final int baseStamp;
    public Base() { Trace.base=this;Trace.hit(8);baseStamp=42;Trace.hit(9); }
}
