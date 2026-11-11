package fixture.yield;
public final class Core extends Base {
    @Override public int yield(int value) { return super.yield(value) ^ this.record; }
    public static int var(int value) { return value - 23; }
    public int sealed(int value) { return this.sealed - value; }
    public int permits(int value) { return this.permits ^ value; }
    public static int call(Core target, int value) {
        return target.yield(value) + target.sealed(value) + target.permits(value)
            + Base.record(value) + Core.var(value);
    }
}
