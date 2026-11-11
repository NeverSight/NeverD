package fixture;
public final class ClosedVisibilityParameterBridgeFuture extends Child {
    @Override public void apply(Child.Left value) {
        Trace.hit(9);
        Trace.futureAction.run();
        super.apply(value);
    }
}
