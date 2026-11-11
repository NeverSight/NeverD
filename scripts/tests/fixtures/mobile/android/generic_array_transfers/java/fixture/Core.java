package fixture;
public class Core {
    public static Object[] move(Object[] source, Object[] destination, int offset, int begin, int end) {
        Object[] result = Provider.move(source, destination, offset, begin, end);
        Trace.result = result; Trace.hit(4);
        return result;
    }
}
