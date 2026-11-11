package fixture;
public final class Payload {
    static { Trace.hit(1); }
    public Object value;
    public Provider receiver;
    public Payload(Object value, Provider receiver) {
        Trace.hit(4);
        this.value = value;
        this.receiver = receiver;
    }
}
