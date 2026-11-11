package fixture;
import java.util.function.Function;
public class Factory {
    public Factory() {}
    @SuppressWarnings("unchecked")
    public static <T,U,A,R> Bucket<T,?,R> map(Function<? super T,? extends U> mapper, Bucket<? super U,A,R> downstream) {
        Trace.calls++;
        Trace.mapper = mapper;
        Trace.downstream = downstream;
        Trace.stage = 1;
        Trace.action.run();
        Trace.stage = 2;
        return (Bucket<T,?,R>)Trace.value;
    }
    public static <T,U,A,R> Bucket<T,?,R> reverse(Bucket<? super U,A,R> downstream, Function<? super T,? extends U> mapper) {
        Trace.reverses++;
        return map(mapper,downstream);
    }
}
