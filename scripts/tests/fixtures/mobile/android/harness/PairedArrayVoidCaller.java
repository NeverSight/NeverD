import fixture.Client;
import fixture.Provider;
import fixture.Trace;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class PairedArrayVoidCaller {
    public static void call() { Client.call(); }
    public static void direct() {
        Trace.hit(9);
        Provider.apply(Trace.sink, Trace.pairs);
        Trace.hit(8);
    }
}
