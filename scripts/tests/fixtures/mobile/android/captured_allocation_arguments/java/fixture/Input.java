package fixture;
public class Input {
    public Object value;
    public Object read() { Trace.hit(2); return value; }
}
