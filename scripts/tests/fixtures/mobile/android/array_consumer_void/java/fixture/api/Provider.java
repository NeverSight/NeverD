package fixture.api;
import fixture.Sink;
import fixture.Trace;
@SuppressWarnings("unchecked")
public class Provider {
    static { Trace.hit(1); }
    public static <T> void apply(T[] values, Sink<? super T> sink, int begin, int end) {
        Trace.hit(5); Trace.observed = values;
        T left = values[begin]; Trace.left = left; Trace.hit(6);
        T right = values[end]; Trace.right = right;
        T replacement = (T)Trace.replacement;
        int decision = sink.compare(left, right);
        Trace.hit(7);
        if (decision <= 0) values[begin] = replacement;
        else values[end] = replacement;
        Trace.hit(8);
    }
}
