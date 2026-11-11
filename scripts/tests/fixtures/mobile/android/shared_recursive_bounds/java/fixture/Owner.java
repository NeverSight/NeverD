package fixture;

public final class Owner {
    public static <T, K extends Comparable<? super K>> Object make(Selector<? super T, ? extends K> selector, K key) {
        return new Worker<T, K>(selector, key);
    }
    public static <T, K extends Comparable<? super K>> Object another(Selector<? super T, ? extends K> selector, K key) {
        return new Worker<T, K>(selector, key);
    }
    public static final class Worker<T, K extends Comparable<? super K>> implements Reader<T, Integer> {
        public final Selector<? super T, ? extends K> selector;
        public final K key;
        public Worker(Selector<? super T, ? extends K> selector, K key) { this.selector = selector; this.key = key; }
        public Integer read(T value) { return Integer.valueOf(Comparisons.compare(selector.select(value), key)); }
        public <K> K echo(K value) { return value; }
    }
}
