package fixture;
public class Core<E> extends Base<Slot> {
    public int calls;
    public int requested;
    public boolean cached;
    public Slot[] value;
    public Slot[] last;
    public RuntimeException failure;
    protected Slot[] allocate(int length) {
        calls++;
        requested = length;
        if (failure != null) throw failure;
        if (!cached) value = new Slot[length];
        last = value;
        return value;
    }
    public Slot[] typed(int length) { return allocate(length); }
    public static Broad<?>[] view(Slot[] values) { return values; }
}
