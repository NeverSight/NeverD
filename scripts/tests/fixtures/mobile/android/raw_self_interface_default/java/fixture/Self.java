package fixture;

@SuppressWarnings("rawtypes")
public interface Self<T> extends Comparable<Self<T>> {
    int rank();
    default int compareTo(Self other) {
        Trace.calls++;
        Trace.seen = other;
        if (Trace.failure != null) throw Trace.failure;
        return other == null ? rank() : rank() - other.rank();
    }
}
