package fixture;
import java.util.function.Supplier;
public class Provider {
    public static <X extends Throwable> void evaluate(Object left,Object right,Supplier<X> errors)throws X {
        Trace.hit(2);Trace.left=left;Trace.right=right;Trace.provider=errors;
        Trace.action.run();if(Trace.reject)throw errors.get();Trace.hit(3);
    }
}
