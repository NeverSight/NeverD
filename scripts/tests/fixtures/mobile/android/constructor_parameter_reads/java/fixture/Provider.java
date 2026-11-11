package fixture;

public class Provider {
    public int read() {
        Trace.receiver = this;
        Trace.calls++;
        Trace.hit(2);
        ((int[]) Trace.input)[0]++;
        Trace.hit(3);
        return Trace.result;
    }
}
