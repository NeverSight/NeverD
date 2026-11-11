package fixture;

public interface Ordered extends Ordering<Ordered> {
    int order();

    @Override
    default int compare(Ordered other) {
        Trace.calls++;
        Trace.events = Trace.events * 10 + 1;
        Trace.receiver = this;
        Trace.argument = other;
        if (Trace.failure != null) throw Trace.failure;
        int left = order();
        int right = other.order();
        Trace.events = Trace.events * 10 + 4;
        return left - right;
    }
}
