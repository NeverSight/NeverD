package fixture;
public class Core extends Parent<Integer> {
    public final String left, right;
    public final int originalFirst, originalSecond;
    public Core(String left, String right, int first, int second, long tag) {
        super(Provider.take(first), Provider.take(second), tag);
        this.left = left;
        this.right = right;
        this.originalFirst = first;
        this.originalSecond = second;
        Trace.hit(6);
    }
    public Core(String left, String right, Integer first, Integer second, long tag) {
        super(first, second, tag);
        this.left = left;
        this.right = right;
        this.originalFirst = 0;
        this.originalSecond = 0;
        Trace.hit(6);
    }
}
