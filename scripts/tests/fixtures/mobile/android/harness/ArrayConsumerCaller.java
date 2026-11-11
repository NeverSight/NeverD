import fixture.Client;
import fixture.Trace;
import fixture.api.Facade;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class ArrayConsumerCaller {
    public static void throughClient(int begin, int end) { Client.call(begin, end); }
    public static void direct(int begin, int end) {
        Trace.hit(2);
        Facade.apply(Trace.current, Trace.sink, begin, end);
        Trace.hit(9);
    }
}
