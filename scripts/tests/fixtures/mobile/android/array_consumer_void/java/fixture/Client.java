package fixture;
import fixture.api.Facade;
@SuppressWarnings({"rawtypes", "unchecked"})
public class Client {
    public static void call(int begin, int end) {
        Trace.hit(2);
        Facade.apply(Trace.current, Trace.sink, begin, end);
        Trace.hit(9);
    }
}
