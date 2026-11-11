package fixture;
public class Value implements Input {
    public void configure(float amount) {
        Trace.hit(7); Trace.configured = this;
        Trace.bits = Float.floatToRawIntBits(amount); Trace.configure.run();
    }
}
