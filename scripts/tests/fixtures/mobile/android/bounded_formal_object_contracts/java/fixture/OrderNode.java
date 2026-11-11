package fixture;

public interface OrderNode extends Ordering<OrderNode> {
    Bound get();

    @Override
    default int compare(OrderNode other) {
        Trace.calls++;
        Trace.events = Trace.events * 10 + 1;
        Trace.receiver = this;
        Trace.argument = other;
        if (Trace.failure != null) throw Trace.failure;
        int left = get().value();
        int right = other.get().value();
        Trace.events = Trace.events * 10 + 4;
        return left - right;
    }
}
