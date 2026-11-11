package fixture;
import java.util.function.Supplier;
public class Core {
    public static void call(Object left,Object right,Supplier<RuntimeException> errors)throws IllegalArgumentException {
        Trace.hit(1);Provider.evaluate(left,right,errors);Trace.hit(4);
    }
    @SuppressWarnings("unchecked")
    public static void checked(Object left,Object right,Object errors)throws IllegalArgumentException {
        Trace.hit(1);Supplier<RuntimeException> argument=(Supplier<RuntimeException>)errors;
        Provider.evaluate(left,right,argument);Trace.hit(4);
    }
}
