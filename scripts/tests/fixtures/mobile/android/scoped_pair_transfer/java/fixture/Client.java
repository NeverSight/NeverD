package fixture;
import java.util.Map;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Client {
    public static Object call(Pair<?,?> pair) { Map result=Provider.make((Pair)pair);Trace.after=result;Trace.hit(6);return result; }
    public static String key(Pair<?,?> pair) { Map<String,?> result=Provider.make((Pair)pair);Trace.after=result;Trace.hit(6);return result.keySet().iterator().next(); }
    public static String value(Pair<?,?> pair) { Map<?,String> result=Provider.make((Pair)pair);Trace.after=result;Trace.hit(6);return result.get(Trace.key); }
}
