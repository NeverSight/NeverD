package fixture;
public class Base {
    public void apply(Payload input) {
        Trace.hit(3); Trace.receiver = this; Trace.argument = input;
        Trace.action.run(); Trace.hit(4);
    }
    public long wide(long input) {
        Trace.hit(3); Trace.receiver = this; Trace.numberArgument = input;
        Trace.action.run(); Trace.hit(4); return Trace.result;
    }
}
