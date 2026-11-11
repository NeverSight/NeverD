package fixture;
import fixture.api.Facade;
@SuppressWarnings({"rawtypes", "unchecked"})
public class Client {
    public static Box call(int begin, int end) {
        Trace.hit(2);
        Box result = Facade.apply(Trace.current, Trace.sink, begin, end);
        Trace.hit(9); return result;
    }
}
