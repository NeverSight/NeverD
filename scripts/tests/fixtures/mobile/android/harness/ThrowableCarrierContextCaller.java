import fixture.Core;
import java.util.function.Supplier;
public final class ThrowableCarrierContextCaller {
    public static void call(Object left,Object right,Supplier<RuntimeException> errors){Core.call(left,right,errors);}
    public static void checked(Object left,Object right,Object errors){Core.checked(left,right,errors);}
}
