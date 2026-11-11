package fixture;
public class Carrier {
    public float[] value;
    public Carrier(float[] value) { this.value = value; }
    public float[] unwrap() {
        Trace.events = Trace.events * 10 + 1;
        if (Trace.firstFailure != null) throw Trace.firstFailure;
        return value;
    }
}
