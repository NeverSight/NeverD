package fixture;
public final class Provider implements Owner {
    public Object owner;
    public Item first;
    public Item second;
    public long bits;
    public int calls;
    public long app() { Trace.event(2); return bits; }
    public Object owner() { Trace.event(3); return owner; }
    public Item current() {
        calls++;
        Trace.event(calls == 1 ? 4 : 5);
        return calls == 1 ? first : second;
    }
}
