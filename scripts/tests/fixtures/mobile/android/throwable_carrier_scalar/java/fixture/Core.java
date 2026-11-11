package fixture;
import java.util.function.Supplier;
public class Core {
    public static double call(double value,double lower,double upper,Supplier<? extends RuntimeException> errors) {
        Trace.hit(1);double result=Provider.evaluate(value,lower,upper,errors);Trace.hit(4);return result;
    }
    @SuppressWarnings("unchecked")
    public static double checked(double value,double lower,double upper,Object errors) {
        Trace.hit(1);Supplier<RuntimeException> argument=(Supplier<RuntimeException>)errors;
        double result=Provider.evaluate(value,lower,upper,argument);Trace.hit(4);return result;
    }
}
