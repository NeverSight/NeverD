package fixture;
public class Input {
    public int bits;
    public Input(int value) { bits = value; }
    public int read() {
        Trace.readOwner = this;
        Trace.hit(2);
        if (Trace.readAction != null) Trace.readAction.run();
        return bits;
    }
}
