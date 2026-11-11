package fixture;

public final class Core implements Api<Object> {
    public long value;
    public int calls;
    public Object marker;
    public Object observed;
    public RuntimeException failure;

    public Core(long value) {
        this.value = value;
    }

    public final long unbox() {
        calls++;
        observed = marker;
        if (marker instanceof int[]) ((int[]) marker)[0] += 3;
        if (failure != null) throw failure;
        return value;
    }

    @Override
    public int read(Object input) {
        Core checked = (Core) input;
        long other = checked.unbox();
        long own = unbox();
        return own == other ? 0 : own < other ? -1 : 1;
    }
}
