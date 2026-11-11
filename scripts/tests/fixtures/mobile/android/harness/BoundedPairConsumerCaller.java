import fixture.*;
public final class BoundedPairConsumerCaller {
    @SuppressWarnings("unchecked")
    public static void call(Source source,State state) { Provider.consume(source,state); }
}
