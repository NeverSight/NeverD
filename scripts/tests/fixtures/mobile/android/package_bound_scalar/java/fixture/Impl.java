package fixture;
final class Impl extends Provider {
    <V extends Bound> int read(V value) {
        Trace.hit(3);
        Trace.receiver = this;
        Trace.argument = value;
        int result = value.number;
        Trace.observed = result;
        Trace.hit(4);
        Trace.action.run();
        Trace.hit(5);
        return result;
    }
}
