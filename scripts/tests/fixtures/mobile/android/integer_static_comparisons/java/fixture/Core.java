package fixture;
public final class Core implements Api {
    public int tag, score, calls;
    public Api seen;
    public RuntimeException failure;
    public Core(int tag) { this.tag = tag; }
    public int observe(Api other) {
        Trace.events = Trace.events * 10 + tag;
        calls++;
        seen = other;
        if (failure != null) throw failure;
        return score;
    }
    public int compareTo(Api other) { return Dispatch.compare(this, other); }
}
