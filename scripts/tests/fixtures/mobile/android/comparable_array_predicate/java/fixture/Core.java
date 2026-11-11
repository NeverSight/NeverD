package fixture;
public final class Core {
    private Core() {}
    public static <Value extends Comparable<? super Value>> boolean call(Value[] values) {
        return Provider.test(values);
    }
    public static boolean checked(Object values) {
        return call((Comparable[]) values);
    }
}
