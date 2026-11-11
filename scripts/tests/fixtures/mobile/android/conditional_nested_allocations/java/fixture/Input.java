package fixture;
public final class Input {
    public String value;
    public String read() { Trace.hit(5); return value; }
}
