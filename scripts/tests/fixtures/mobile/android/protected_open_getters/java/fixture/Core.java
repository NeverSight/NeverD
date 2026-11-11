package fixture;
public class Core<E> extends Base<Slot> {
    public int calls;
    public int marker;
    public boolean allocate;
    public Slot value;
    public Slot last;
    public RuntimeException failure;
    protected Slot get() {
        calls++;
        if (failure != null) throw failure;
        if (allocate) value = new Slot(marker);
        last = value;
        return value;
    }
    public Slot typed() { return get(); }
}
