package fixture;
import api.Function;
import kotlin.jvm.internal.DefaultConstructorMarker;
public final class Facade {
    public static Object create(long bits,Context context,float weight,Token token,Function callback,DefaultConstructorMarker marker) {
        return new Core(bits,context,weight,token,callback,marker);
    }
}
