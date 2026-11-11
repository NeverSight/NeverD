package fixture;
public final class Target {
    static { Trace.hit(8); }
    public final int first;
    public final Object key;
    public final int branch;
    public final int next;
    public Target(int first, Object key, int branch, int next) {
        Trace.seenTarget = this;
        Trace.seenFirst = first;
        Trace.seenKey = key;
        Trace.seenBranch = branch;
        Trace.seenNext = next;
        Trace.mask |= 2;
        Trace.hit(3);
        this.first = first;
        this.key = key;
        this.branch = branch;
        this.next = next;
        Trace.mask |= 4;
        Trace.hit(4);
        Trace.result = this;
    }
}
