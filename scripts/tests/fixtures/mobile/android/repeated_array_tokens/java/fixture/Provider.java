package fixture;
import java.util.Collection;
public class Provider extends Base {
    public Provider() {}
    @SuppressWarnings("unchecked")
    public static <T> T[] read(Collection<T> source, Class<T> token) {
        Trace.calls++;
        Trace.source = source;
        Trace.token = token;
        Trace.stage = 1;
        Trace.name = token.getName();
        Trace.size = source.size();
        Trace.stage = 2;
        Trace.action.run();
        Trace.stage = 3;
        return (T[])Trace.value;
    }
    public static <T> T[] reverse(Class<T> token, Collection<T> source) {
        Trace.relays++;
        return read(source,token);
    }
}
