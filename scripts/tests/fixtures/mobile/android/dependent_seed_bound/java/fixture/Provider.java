package fixture;
import java.util.function.Supplier;
public class Provider {
    @SuppressWarnings("unchecked")
    public static <E,T extends Iterable<E>> T context(T value,String message,Object... data)throws IllegalArgumentException {
        Trace.hit(2);Trace.value=value;Trace.message=message;Trace.data=data;Trace.action.run();Trace.hit(3);return (T)Trace.result;
    }
    @SuppressWarnings("unchecked")
    public static <E,T extends Iterable<E>,X extends Throwable> T throwing(T value,Supplier<X> errors)throws X {
        Trace.hit(2);Trace.value=value;Trace.provider=errors;Trace.action.run();if(Trace.reject)throw errors.get();Trace.hit(3);return (T)Trace.result;
    }
}
