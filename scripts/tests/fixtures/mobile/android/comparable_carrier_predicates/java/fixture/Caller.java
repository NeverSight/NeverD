package fixture;
@SuppressWarnings("rawtypes")
public final class Caller {
    public static boolean contains(Core owner, Comparable value) { return owner.contains(value); }
    public static boolean before(Core owner, Comparable left, Comparable right) { return owner.before(left, right); }
    public static Comparable get(Core owner) { return owner.get(); }
}
