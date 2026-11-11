package fixture;
public final class Provider<C> {
    public static <T> Result make(Pair<? extends Carrier<T>,? extends T> pair) {
        Trace.hit(1);Trace.pair=pair;
        Carrier<T> carrier=pair.first;Trace.carrier=carrier;Trace.hit(2);
        Trace.seed=pair.second;Trace.hit(3);
        Trace.extracted=carrier.read();Trace.hit(4);
        return (Result)Trace.reply;
    }
}
