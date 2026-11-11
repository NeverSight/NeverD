import fixture.Core;
import java.util.function.Supplier;
public final class ThrowableArrayCarrierCaller {
    public static String[] call(String[] values,Supplier<RuntimeException> errors){return Core.call(values,errors);}
    public static String[] checked(Object values,Object errors){return (String[])Core.checked(values,errors);}
}
