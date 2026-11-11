package fixture;
import java.util.*;
import java.util.function.*;
public class Provider {
    public static <T,U> Comparator<T> create(Function<? super T,? extends U> fn,U... keys) { return create(fn,false,keys); }
    @SuppressWarnings("unchecked")
    public static <T,U> Comparator<T> create(Function<? super T,? extends U> fn,boolean flag,U... keys) {
        Trace.hit(1);Trace.fn=fn;Trace.keys=keys;Trace.flag=flag;
        Trace.action.run();Trace.hit(2);return (Comparator<T>)Trace.result;
    }
}
