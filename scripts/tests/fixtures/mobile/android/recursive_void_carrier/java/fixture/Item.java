package fixture;
public final class Item implements Comparable {
    public final int number, id;
    public Item(int number, int id) { this.number = number; this.id = id; }
    public int compareTo(Object other) {
        Item value = (Item) other;
        Trace.pairs = Trace.pairs * 100 + id * 10 + value.id;
        Trace.comparisons++;
        Trace.hit(2);
        return Integer.compare(number, value.number);
    }
}
