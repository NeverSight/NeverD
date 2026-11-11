package fixture;

public class Counter<T> extends Middle<T,String> implements CountView<T> {
    public Counter() {}

    public boolean contains(Element<T> value) {
        return super.contains(value);
    }

    @SuppressWarnings("unchecked")
    public final boolean contains(Object value) {
        if (!(value instanceof Element)) return false;
        Element<T> element = (Element<T>) value;
        return contains(element);
    }
}
