package fixture;
public class Carrier {
    public Payload value;
    public Carrier(Payload value) { this.value = value; }
    public Payload unwrap() {
        Trace.events = Trace.events * 10 + 1;
        if (Trace.firstFailure != null) throw Trace.firstFailure;
        return value;
    }
}
