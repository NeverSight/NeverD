package fixture;
public final class PackageBoundScalarFuture extends Provider {
    <W extends Bound> int read(W value) {
        Trace.hit(9);
        Trace.hit(3);
        Trace.receiver = this;
        Trace.argument = value;
        int result = value.number;
        Trace.observed = result;
        Trace.hit(4);
        Trace.action.run();
        Trace.hit(5);
        return result ^ 0x40000000;
    }
}
