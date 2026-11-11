package fixture;
import java.util.List;
public final class Provider<T> {
    static { Trace.hit(1); }
    public static <T> int scan(List<? extends T> values,int from,int to,Comparison<? super T,Integer> comparison) {
        Trace.hit(2);
        for(int i=from;i<to;i++) {
            T value=values.get(i);Trace.current=value;Trace.hit(3);
            int rank=comparison.apply(value).intValue();
            Trace.sum+=rank;Trace.hit(5);
        }
        Trace.hit(6);return Trace.result;
    }
}
