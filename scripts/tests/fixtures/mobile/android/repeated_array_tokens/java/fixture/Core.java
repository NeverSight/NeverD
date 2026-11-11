package fixture;
import java.util.Collection;
public final class Core {
    private Core() {}
    public static Object[] raw(Collection<Object> source, Class<Object> token) { return Provider.read(source,token); }
    public static String[] narrow(Collection<Object> source, Class<Object> token) { return (String[])Provider.read(source,token); }
    public static Object ignored(Collection<Object> source, Class<Object> token) { Provider.read(source,token); return Trace.marker; }
    public static Object[] reverse(Collection<Object> source, Class<Object> token) { return Provider.reverse(token,source); }
    public static <T> T[] typed(Collection<T> source, Class<T> token) { return Provider.read(source,token); }
    public static Object[] alias(Collection<Object> source, Class<Object> token) { return Facade.read(source,token); }
}
