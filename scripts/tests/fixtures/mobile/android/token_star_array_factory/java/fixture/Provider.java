package fixture;
import java.lang.reflect.Constructor;
public class Provider {
    public static <Entry> Constructor<Entry> select(Class<Entry> type, Class<?>... parts) {
        Trace.type=type;Trace.parts=parts;Trace.hit(1);
        Trace.action.run();Trace.hit(2);
        return (Constructor<Entry>)Trace.result;
    }
    public static Constructor select(Object type, Object... parts) {
        Trace.hit(9);return null;
    }
}
