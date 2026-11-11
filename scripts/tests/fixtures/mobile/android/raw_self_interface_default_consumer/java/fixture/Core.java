package fixture;

public class Core<T> implements Self<T> {
    public final int score;
    public final Object payload;
    public RuntimeException problem;
    public Core(int score, Object payload) { this.score = score; this.payload = payload; }
    public int rank() {
        Trace.ranks++;
        if (problem != null) throw problem;
        return score;
    }
}
