package fixture;
public class Counter<T> implements CountView<T> {
    public Element<T> expected;
    public Element<T> last;
    public int calls;
    public RuntimeException failure;
    public Counter() {}
    public boolean contains(Element<T> value) {
        calls++;
        last = value;
        if (failure != null) throw failure;
        return value == expected;
    }
    @SuppressWarnings("unchecked")
    public final boolean contains(Object value) {
        if (!(value instanceof Element)) return false;
        Element<T> element = (Element<T>) value;
        return contains(element);
    }
}
