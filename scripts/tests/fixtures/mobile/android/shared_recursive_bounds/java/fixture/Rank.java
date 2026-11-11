package fixture;

public final class Rank implements Comparable<Rank> {
    public int value;
    public int calls;
    public RuntimeException failure;
    public Rank(int value) { this.value = value; }
    public int compareTo(Rank other) {
        ++calls;
        if (failure != null) throw failure;
        return value < other.value ? -1 : value == other.value ? 0 : 1;
    }
}
