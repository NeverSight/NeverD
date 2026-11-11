package fixture;

import java.util.Comparator;

public final class Owner<T> {
    public T current;
    public final class Receiver<R> implements Comparator<T> {
        public T seen;
        public int compare(T left, T right) {
            seen = left;
            Owner.this.current = right;
            Trace.calls++;
            Trace.left = left;
            Trace.right = right;
            if (left == Trace.failure && Trace.problem != null) throw Trace.problem;
            if (left == right) return 0;
            if (left == null) return -1;
            return 1;
        }
    }
    public Receiver<String> create() {
        return new Receiver<String>();
    }
}
