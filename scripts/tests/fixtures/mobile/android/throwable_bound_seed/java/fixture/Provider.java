package fixture;
import java.util.function.Supplier;
public class Provider {
    @SuppressWarnings("unchecked")
    public static <T extends CharSequence,X extends Throwable> T evaluate(T value,Supplier<X> errors)throws X {
        Trace.hit(2);Trace.value=value;Trace.provider=errors;Trace.action.run();
        if(Trace.reject)throw errors.get();Trace.hit(3);return (T)Trace.result;
    }
}
