package fixture;

public class Counter<T> extends Middle<Holder<T>> implements CountView<Holder<T>> {
    public Counter() {}
    public Holder<T> getOrDefault(Element<T> key, Holder<T> fallback) {
        return super.getOrDefault(key, fallback);
    }
    @SuppressWarnings("unchecked")
    public final Holder<T> getOrDefault(Object key, Holder<T> fallback) {
        if (!(key instanceof Element<?>)) return fallback;
        return getOrDefault((Element<T>) key, fallback);
    }
    @SuppressWarnings("unchecked")
    public Object erasedDefault(Object key, Object fallback) {
        if (!(key instanceof Element<?>)) return fallback;
        return getOrDefault((Element<T>) key, (Holder<T>) fallback);
    }
}
