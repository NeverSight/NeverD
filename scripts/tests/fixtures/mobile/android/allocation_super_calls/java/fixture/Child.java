package fixture;
public class Child extends Core {
    public Object read() { Trace.hit(9); return Trace.replacement; }
    public int number() { Trace.hit(9); return Trace.afterNumber; }
}
