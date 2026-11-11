package fixture;

public final class Comparisons {
    private Comparisons() { }
    public static <K extends Comparable<? super K>> int compare(K first, K second) {
        if (first == second) return 0;
        if (first == null) return -1;
        if (second == null) return 1;
        return first.compareTo(second);
    }
}
