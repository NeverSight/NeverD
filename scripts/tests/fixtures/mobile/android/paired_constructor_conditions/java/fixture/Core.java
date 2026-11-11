package fixture;
public final class Core {
    public final Object value;
    public Core(Factory factory, int start, int bound, Object left, Object right) {
        Object value = left;
        int index = start;
        while (index < bound) {
            value = factory.step(value, index);
            index++;
        }
        if (start == bound) factory.stage(10, value);
        if (start != bound) factory.stage(11, value);
        if (start < bound) factory.stage(12, value);
        if (start >= bound) factory.stage(13, value);
        if (start > bound) factory.stage(14, value);
        if (start <= bound) factory.stage(15, value);
        if (left == right) factory.stage(16, value);
        if (left != right) factory.stage(17, value);
        this(value);
    }
    private Core(Object value) {
        Trace.hit(2, this);
        this.value = value;
        Trace.core = this;
        Trace.hit(3, this);
    }
}
