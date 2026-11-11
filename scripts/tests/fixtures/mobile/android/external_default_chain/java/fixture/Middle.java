package fixture;

public interface Middle<M> extends Root<M> {
    @Override default Middle<M> reverse() {
        Trace.middleReverse++;
        if (Trace.failure != null) throw Trace.failure;
        return this;
    }
    default int fallback(int input) {
        Trace.middle++;
        Trace.lastOwner = 20;
        if (Trace.failure != null) throw Trace.failure;
        return input + 20;
    }
}
