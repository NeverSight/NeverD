package fixture;
public class Value implements First, Second {
    public void first() { Trace.firstCalls++; Trace.hit(3); }
    public void second() { Trace.secondCalls++; Trace.hit(5); }
}
