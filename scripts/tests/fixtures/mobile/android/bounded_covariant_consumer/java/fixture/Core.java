package fixture;
public final class Core {
    private Core() {}
    public static void call(Consumer consumer, Carrier input) {
        Consumer.events = Consumer.events * 5 + 3;
        consumer.accept(input);
        Consumer.events = Consumer.events * 5 + 4;
    }
    public static <R extends Bound> void typed(Consumer consumer, Carrier<? extends R> input) {
        Consumer.events = Consumer.events * 5 + 3;
        consumer.accept(input);
        Consumer.events = Consumer.events * 5 + 4;
    }
    public static void direct(Consumer consumer, Carrier input) { consumer.accept(input); }
}
