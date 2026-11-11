package fixture;
@SuppressWarnings({"rawtypes","unchecked"})
public class Provider {
    static { Trace.hit(6); }
    public static <T,K extends Comparable<? super K>> Box<T> choose(Box<T> seed,Callback<? super T,? extends K> selector) {
        Trace.seenSeed = seed; Trace.hit(1);
        T value = seed.value; Trace.seenInput = value; Trace.hit(2);
        K key = selector.apply(value); Trace.seenKey = key; Trace.hit(3);
        int result = key.compareTo(key); Trace.comparison = result; Trace.hit(4);
        Box<T> box = (Box<T>)Trace.result; Trace.hit(5);
        return box;
    }
}
