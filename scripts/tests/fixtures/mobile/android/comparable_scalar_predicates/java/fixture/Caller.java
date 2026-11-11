package fixture;
public final class Caller {
    public static boolean call(Core receiver, Comparable<?> value) { return receiver.contains(value); }
    public static boolean typed(Api<Character> receiver, Character value) { return receiver.contains(value); }
}
