package fixture;
import java.util.Map;
public final class Core {
    @SuppressWarnings("unchecked")
    public static Map call(Map input,boolean reverse) {return Provider.transfer(input,reverse);}
    public static Map checked(Object input,boolean reverse) {return call((Map)input,reverse);}
}
