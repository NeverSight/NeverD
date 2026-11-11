import fixture.Core;
import java.util.function.Supplier;
public final class ThrowableCarrierScalarCaller {
    public static double call(double value,double lower,double upper,Supplier<? extends RuntimeException> errors){return Core.call(value,lower,upper,errors);}
    public static double checked(double value,double lower,double upper,Object errors){return Core.checked(value,lower,upper,errors);}
}
