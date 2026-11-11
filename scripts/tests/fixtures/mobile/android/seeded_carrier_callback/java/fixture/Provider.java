package fixture;
import java.util.List;
public final class Provider {
    @SuppressWarnings("unchecked") public static <A,B> Target<B> adapt(List<A> values,B seed,Context context,Sink<A,B> sink) {
        Trace.hit(1);
        Trace.values=values;
        Trace.seed=seed;
        Trace.context=context;
        Trace.sink=sink;
        Trace.action.run();
        Trace.hit(2);
        sink.accept(values==null?null:values.get(0),(Target<B>)Trace.target);
        Trace.hit(3);
        return (Target<B>)Trace.result;
    }
}
