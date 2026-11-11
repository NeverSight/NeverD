package fixture;
public final class Core {
    public volatile int[] values;
    public volatile int[] flags;
    public volatile Object token;
    private Object read(int index) {
        Object captured = token;
        Trace.seenArray = values;
        Trace.seenIndex = index;
        Trace.hit(1);
        values[index] = Trace.replacementValue;
        values = Trace.afterValues;
        flags = Trace.afterFlags;
        token = Trace.replacement;
        Trace.mask |= 1;
        Trace.hit(2);
        Trace.seenReturn = captured;
        return captured;
    }
    public Target make(int index) {
        return new Target(values[index], read(index), flags[index] > 0 ? flags[index] : values[index], index + 1);
    }
}
