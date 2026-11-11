package fixture;
public final class Target {
    static { Trace.step(1); Trace.holder.value = Trace.initialValue; Trace.step(2); }
    public String[] left;
    public int[] right;
    public Target(String[] left, int[] right) {
        Trace.seen = this; Trace.first = left; Trace.second = right;
        Trace.firstValue = left.length == 0 ? null : left[0];
        Trace.number = right[0];
        Trace.step(5);
        this.left = left; this.right = right;
        Trace.step(6);
        if (left.length > 0) left[0] = Trace.afterValue;
        Trace.step(7);
    }
}
