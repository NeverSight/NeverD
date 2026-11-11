package fixture;
public class Core<E> extends Base<Event> {
    public int calls;
    public int marker;
    public Event last;
    public RuntimeException failure;
    protected void accept(Event value) {
        calls++;
        last = value;
        if (failure != null) throw failure;
        if (value != null) value.value = marker;
    }
    public void typed(Event value) { accept(value); }
}
