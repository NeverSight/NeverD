package fixture;
public final class Core {
    private Core() {}
    public static void call(Consumer consumer,Element[] values,int flags) {
        Trace.hit(0, consumer, values, null, flags);
        consumer.accept(values, flags);
    }
    public static void typed(Consumer consumer,Element[] values,int flags) {
        Trace.hit(0, consumer, values, null, flags);
        consumer.<Element>accept(values, flags);
    }
}
