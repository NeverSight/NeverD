package fixture;
public class Base {
    public Object read() { Trace.baseReceiver = this; Trace.hit(3); return Trace.value; }
    public int number() { Trace.baseReceiver = this; Trace.hit(4); return Trace.number; }
}
