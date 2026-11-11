package fixture;
public class Carrier {
    public int value;
    public Carrier(int value) { this.value = value; }
    public int unwrap() {
        Trace.events = Trace.events * 10 + 1;
        if (Trace.firstFailure != null) throw Trace.firstFailure;
        return value;
    }
}
