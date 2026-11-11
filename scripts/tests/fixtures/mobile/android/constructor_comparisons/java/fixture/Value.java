package fixture;
public final class Value {
    public final double input;
    public Value(double input) {
        Trace.created=this;Trace.allocations++;Trace.step(5);
        this.input=input;Trace.step(6);
    }
}
