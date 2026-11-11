import fixture.Box;
import fixture.Client;
import fixture.Trace;
import fixture.api.Facade;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class ArrayCarrierCaller {
    public static Box throughClient(int begin, int end) { return Client.call(begin, end); }
    public static Box direct(int begin, int end) {
        Trace.hit(2);
        Box result = Facade.apply(Trace.current, Trace.sink, begin, end);
        Trace.hit(9); return result;
    }
}
