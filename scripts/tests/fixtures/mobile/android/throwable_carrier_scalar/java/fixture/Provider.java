package fixture;
import java.util.function.Supplier;
public class Provider {
    public static <X extends Throwable> double evaluate(double value,double lower,double upper,Supplier<? extends X> errors)throws X {
        Trace.hit(2);Trace.value=value;Trace.lower=lower;Trace.upper=upper;Trace.provider=errors;
        Trace.action.run();
        if(Trace.reject)throw errors.get();
        Trace.hit(3);return Trace.result;
    }
}
