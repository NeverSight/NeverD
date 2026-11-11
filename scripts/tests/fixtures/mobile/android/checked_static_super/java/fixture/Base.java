package fixture;
public class Base {
    public void ping() { Trace.last = this; Trace.event(4); }
}
