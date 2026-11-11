package fixture;
import java.util.Comparator;
import java.util.function.Function;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class Factory<T> {
    private Factory() {}
    static {
        Trace.initializations++;
        if (Trace.failInitialization) throw Trace.initializationFailure;
    }
    public static <T,U extends Comparable<? super U>> Comparator<T> create(Function<? super T,? extends U> selector) {
        Trace.calls++;
        Trace.stage = 1;
        T input = (T)Trace.input;
        Trace.seenInput = input;
        U key = selector.apply(input);
        Trace.key = key;
        Trace.stage = 2;
        Trace.comparison = key.compareTo(key);
        Trace.stage = 3;
        return (Comparator<T>)Trace.result;
    }
}
