import fixture.Core;
import java.util.function.Supplier;
public final class ThrowableBoundSeedCaller {
    public static String call(String value,Supplier<RuntimeException> errors){return Core.call(value,errors);}
    public static String checked(Object value,Object errors){return (String)Core.checked(value,errors);}
}
