package fixture;
public abstract class Family<B extends Family<B>> {
    protected static class Hidden extends Base {
        public Hidden() { }
        public void apply(Payload input) { super.apply(input); }
        public long wide(long input) { return super.wide(input); }
    }
}
