package fixture;

public interface Other extends Ordering<Ordered> {
    @Override
    default int compare(Ordered other) {
        Trace.events = Trace.events * 10 + 8;
        return other.order();
    }
}
