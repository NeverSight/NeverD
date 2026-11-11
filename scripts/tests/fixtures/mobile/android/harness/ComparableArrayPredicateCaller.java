package fixture;
public final class ComparableArrayPredicateCaller {
    public static <Cell extends Comparable<? super Cell>> boolean call(Cell[] values) {
        return Core.call(values);
    }
    public static boolean checked(Object values) { return Core.checked(values); }
}
