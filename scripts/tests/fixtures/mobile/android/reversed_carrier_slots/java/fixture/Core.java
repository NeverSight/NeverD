package fixture;
import java.util.Map;
public final class Core {
    @SuppressWarnings("unchecked") public static Map call(Map input){return Provider.reverse(input);}
    public static Map checked(Object input){return call((Map)input);}
}
