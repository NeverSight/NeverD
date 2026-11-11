package fixture;
public final class Caller {
    public static boolean call(Core receiver, Comparable<?> value) { return receiver.contains(value); }
    public static boolean typed(Api<Double> receiver, Double value) { return receiver.contains(value); }
    public static boolean pair(Core receiver, Comparable<?> left, Comparable<?> right) { return receiver.before(left, right); }
    public static boolean typedPair(Api<Double> receiver, Double left, Double right) { return receiver.before(left, right); }
}
