package fixture;
public class Receiver extends Owner {
    @Override public int transform(int value) {
        Trace.transformOwner = this;
        Trace.hit(3);
        if (Trace.transformAction != null) Trace.transformAction.run();
        return value ^ Trace.mask;
    }
    @Override public int side() { return 42; }
}
