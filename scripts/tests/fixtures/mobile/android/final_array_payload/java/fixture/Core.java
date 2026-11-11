package fixture;
public class Core {
    public static final int[] VALUE = {19, -3, 0, 42, 7, 2};
    public static int read(int index, int[] alias) {
        if (alias != VALUE) throw new IllegalArgumentException();
        return VALUE[index];
    }
}
