package fixture;
import java.util.LinkedHashMap;
public class Provider {
    public static <Entry extends Enum<Entry>> LinkedHashMap<String,Entry> collect(Class<Entry> type) {
        Trace.type = type; Trace.hit(1);
        Trace.action.run(); Trace.hit(2);
        return (LinkedHashMap<String,Entry>) Trace.result;
    }
}
