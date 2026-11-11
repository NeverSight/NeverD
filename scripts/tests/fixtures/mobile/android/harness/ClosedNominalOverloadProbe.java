import fixture.*;
import kotlin.jvm.functions.Function2;
public class ClosedNominalOverloadProbe implements Handler,Function2 {
    public Object apply(Object value){Trace.unexpected++;throw new AssertionError("owned unexpected handler");}
    public Object invoke(Object owner,Object completion){Trace.unexpected++;throw new AssertionError("owned unexpected callback");}
    public static final class Child extends ClosedNominalOverloadProbe {}
}
