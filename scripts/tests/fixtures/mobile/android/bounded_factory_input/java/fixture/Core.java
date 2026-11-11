package fixture;

public final class Core {
    private Core() {}
    public static <T extends Base> Factory<Leaf> call(
            Producer<? super Context, ? extends T> producer, Config config, long bits) {
        Trace.hit(1, producer, config, bits, null);
        Factory<Leaf> result = make(producer, config, bits);
        Trace.hit(5, producer, config, bits, result);
        return result;
    }
    private static <T extends Base> Factory<Leaf> make(
            Producer<? super Context, ? extends T> producer, Config config, long bits) {
        Trace.hit(2, producer, config, bits, null);
        T value = producer.produce(Trace.argument(config));
        Trace.hit(3, producer, config, bits, value);
        Factory<Leaf> result = Trace.finish(value, config, bits);
        Trace.hit(4, producer, config, bits, result);
        return result;
    }
}
