package fixture;
public class Core {
    public static Object[] fill(Object[] values, Object value, int begin, int end) {
        Provider.fill(values, value, begin, end);
        Trace.result = values; Trace.hit(4);
        return values;
    }
}
