package fixture;
public class Counter<T> implements CountView<T> {
    public Element<T> expected;
    public Element<T> last;
    public int calls;
    public int result;
    public RuntimeException failure;
    public Counter() {}
    public int indexOf(Element<T> value) {
        calls++;
        last = value;
        if (failure != null) throw failure;
        return value == expected ? result : -9;
    }
    @SuppressWarnings("unchecked")
    public final int indexOf(Object value) {
        if (!(value instanceof Element)) return -1;
        Element<T> element = (Element<T>) value;
        return indexOf(element);
    }
}
