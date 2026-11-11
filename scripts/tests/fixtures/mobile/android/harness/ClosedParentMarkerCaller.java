import api.Function;
import fixture.*;
import kotlin.jvm.internal.DefaultConstructorMarker;
public final class ClosedParentMarkerCaller {
    public static Object call(long bits,Context context,float weight,Token token,Function callback,DefaultConstructorMarker marker) {
        return Facade.create(bits,context,weight,token,callback,marker);
    }
}
