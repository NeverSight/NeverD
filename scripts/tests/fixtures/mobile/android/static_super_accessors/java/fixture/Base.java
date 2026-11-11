package fixture;
public class Base {
    static { Trace.hit(8); }
    public void ping() {
        Trace.calls++;
        Trace.receiver = this;
        Trace.hit(2);
    }
}
