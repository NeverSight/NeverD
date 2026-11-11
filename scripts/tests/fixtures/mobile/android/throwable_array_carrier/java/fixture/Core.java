package fixture;
import java.util.function.Supplier;
public class Core {
    public static <A> A[] call(A[] values,Supplier<RuntimeException> errors) {
        Trace.hit(1);A[] result=Provider.evaluate(values,errors);Trace.hit(4);return result;
    }
    @SuppressWarnings("unchecked")
    public static Object[] checked(Object values,Object errors) {
        Trace.hit(1);Object[] argument=(Object[])values;Supplier<RuntimeException> supplier=(Supplier<RuntimeException>)errors;
        Object[] result=Provider.evaluate(argument,supplier);Trace.hit(4);return result;
    }
}
