package fixture;
public final class CarrierB {
    public long value;
    public CarrierB(long value) { this.value = value; }
    public long unwrap() {
        Trace.events = Trace.events * 10 + 2;
        if (Trace.secondFailure != null) throw Trace.secondFailure;
        return value;
    }
    public static CarrierB wrap(long value) {
        Trace.events = Trace.events * 10 + 4;
        if (Trace.boxFailure != null) throw Trace.boxFailure;
        return new CarrierB(value);
    }
}
