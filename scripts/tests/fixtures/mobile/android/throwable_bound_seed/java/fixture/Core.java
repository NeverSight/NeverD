package fixture;
import java.util.function.Supplier;
public class Core {
    public static <A extends CharSequence> A call(A value,Supplier<RuntimeException> errors) {
        Trace.hit(1);A result=Provider.evaluate(value,errors);Trace.hit(4);return result;
    }
    @SuppressWarnings("unchecked")
    public static CharSequence checked(Object value,Object errors) {
        Trace.hit(1);CharSequence argument=(CharSequence)value;Supplier<RuntimeException> supplier=(Supplier<RuntimeException>)errors;
        CharSequence result=Provider.evaluate(argument,supplier);Trace.hit(4);return result;
    }
}
