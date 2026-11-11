import fixture.Core;
import java.util.Calendar;
public final class PrivateInterfaceContextCaller {
    public static Appendable call(Core receiver,Calendar context,Appendable value){return receiver.call(context,value);}
    public static StringBuilder boxed(Core receiver,Calendar context,StringBuilder value){return receiver.boxed(context,value);}
}
