package fixture;
import java.util.Map;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Client {
    public static Map call(Map first,Map second) { Map result=Provider.merge(first,second);Trace.after=result;Trace.hit(7);return result; }
}
