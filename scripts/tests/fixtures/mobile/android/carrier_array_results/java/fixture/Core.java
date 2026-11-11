package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public class Core {
    public static String[] read(Facade self, int begin, int end, Class token) {
        Object[] result = self.read(begin, end, token);
        Trace.result = result; Trace.hit(4); return (String[]) result;
    }
}
