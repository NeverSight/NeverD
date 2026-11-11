package fixture;
public class Value extends Bound implements Flag {
    public void touch() { Trace.hit(5); Trace.touchAction.run(); }
}
