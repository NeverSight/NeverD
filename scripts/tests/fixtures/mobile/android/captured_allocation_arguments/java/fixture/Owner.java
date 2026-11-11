package fixture;
public class Owner {
    public Input input;
    public Child make(Object tag) { return new Child(input.read(), tag); }
    public Child conditional(Input input, Object tag) {
        return new Child(input == null ? null : input.read(), tag);
    }
    public class Child extends Base {
        public final Object value;
        public Child(Object value, Object tag) {
            super(tag);
            Trace.hit(5); this.value = value; Trace.child = this; Trace.hit(6);
        }
        @Override public Object outer() { return Owner.this; }
    }
}
