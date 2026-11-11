package fixture;
abstract class Base<P extends Input> extends Root {
    protected Base(P first, Input second) {
        Trace.hit(4); Trace.firstSeen = first;
        Trace.base.run(); Trace.secondSeen = second; Trace.hit(5);
    }
}
