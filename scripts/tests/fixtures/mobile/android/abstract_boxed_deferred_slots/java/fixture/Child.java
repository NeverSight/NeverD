package fixture;
import java.util.NoSuchElementException;

public final class Child extends Core {
    public Child(long[] values) { super(values); }
    @Override public boolean hasNext() { return index < values.length; }
    @Override public long readValue() {
        calls++;
        observed = marker;
        if (marker instanceof int[]) ((int[]) marker)[0] += 3;
        if (failure != null) throw failure;
        if (index >= values.length) throw new NoSuchElementException("owned-end");
        return values[index++];
    }
}
