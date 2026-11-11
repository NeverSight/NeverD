package fixture;

import java.util.Comparator;

public final class Receiver<T> implements Comparator<T> {
    public int compare(T left, T right) {
        Trace.calls++;
        Trace.left = left;
        Trace.right = right;
        if (left == Trace.failure && Trace.problem != null) throw Trace.problem;
        if (left == right) return 0;
        if (left == null) return -1;
        if (right == null) return 1;
        return 1;
    }
}
