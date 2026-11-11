package fixture;
public final class Item {
    public Object value;
    public Item(Object input) { value = input; }
    public Object extras() { Trace.event(6); return value; }
}
