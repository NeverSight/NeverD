package fixture;
import java.util.List;
public class Provider {
    @SuppressWarnings("unchecked")
    public static <T extends Bound> List<T> fill(Object context,int index,List<T> values,Object extra,long bits,Fn<? super Integer,? extends T> factory) {
        Trace.context=context;Trace.extra=extra;Trace.seed=values;Trace.factory=factory;Trace.bits=bits;Trace.hit(1);
        T item=factory.apply(Integer.valueOf(index));
        Trace.item=item;Trace.hit(3);
        values.add(item);Trace.hit(4);
        T observed=values.get(0);Trace.observed=observed;Trace.observedValue=observed.value();Trace.hit(5);
        List<T> result=(List<T>)Trace.resultInput;
        Trace.result=result;Trace.hit(6);return result;
    }
}
