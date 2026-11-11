package fixture;
public final class CarrierA {
    public long value;
    public CarrierA(long value) { this.value = value; }
    public long unwrap() {
        Trace.events = Trace.events * 10 + 1;
        if (Trace.firstFailure != null) throw Trace.firstFailure;
        return value;
    }
}
