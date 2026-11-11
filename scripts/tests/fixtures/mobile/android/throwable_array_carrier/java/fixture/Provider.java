package fixture;
import java.util.function.Supplier;
public class Provider {
    @SuppressWarnings("unchecked")
    public static <T,X extends Throwable> T[] evaluate(T[] values,Supplier<X> errors)throws X {
        Trace.hit(2);Trace.values=values;Trace.provider=errors;Trace.action.run();
        if(Trace.reject)throw errors.get();Trace.hit(3);return (T[])Trace.result;
    }
}
