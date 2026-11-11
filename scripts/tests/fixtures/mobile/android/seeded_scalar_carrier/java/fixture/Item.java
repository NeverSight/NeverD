package fixture;
public final class Item implements Comparable<Object> {
    public final int value;
    public Item(int value) { this.value = value; }
    public int compareTo(Object needle) {
        Trace.hit(3);
        int[] key = (int[]) needle;
        key[1]++;
        return Integer.compare(value, key[0]);
    }
}
