package fixture;
class Provider {
    @SuppressWarnings("unchecked")
    public static final <E extends Comparable<? super E>> E select(E left, E right) {
        Trace.hit(1); Trace.left = left; Trace.right = right;
        Trace.hit(2); Trace.action.run(); Trace.hit(3);
        return (E)Trace.result;
    }
}
