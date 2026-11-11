package fixture;
public class Consumer {
    public Consumer() {}
    @SuppressWarnings("unchecked")
    public <T extends Element> void accept(T[] values,int flags) {
        Trace.hit(1, this, values, null, flags);
        T item = (T)Trace.next;
        Trace.hit(2, this, values, item, flags);
        values[flags] = item;
        Trace.hit(3, this, values, item, flags);
    }
}
