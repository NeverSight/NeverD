package fixture;

public class Counter<T> extends Middle<Holder<T>> implements CountView<Holder<T>> {
    public Counter() {}

    public Holder<T> get(Element<T> value) {
        return super.get(value);
    }

    @SuppressWarnings("unchecked")
    public final Holder<T> get(Object value) {
        if (!(value instanceof Element)) return null;
        Element<T> element = (Element<T>) value;
        return get(element);
    }

    // The owned input encoder uses this authored body for the JVM-only
    // erased return entry and removes this template before fixing its baseline.
    @SuppressWarnings("unchecked")
    public final Object erasedGet(Object value) {
        if (!(value instanceof Element)) return null;
        Element<T> element = (Element<T>) value;
        return get(element);
    }
}
