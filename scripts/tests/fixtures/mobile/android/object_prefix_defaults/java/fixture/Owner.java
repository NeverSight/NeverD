package fixture;
final class Owner implements View {
    private boolean zeroFlag;
    private long zeroBits;
    private Object[] zeroArray;
    private final Fn<? super Input, Unit> callback;
    public Owner(Fn<? super Input, Unit> callback) {
        Trace.guard(callback, "owned");
        super();
        this.callback = callback;
        Trace.publish(this);
    }
    public boolean flag() { return zeroFlag; }
    public long bits() { return zeroBits; }
    public Object[] cache() { return zeroArray; }
    public Object payload() { return callback; }
}
