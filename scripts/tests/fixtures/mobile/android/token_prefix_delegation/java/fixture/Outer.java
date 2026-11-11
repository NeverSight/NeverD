package fixture;
public class Outer {
    public static class Child extends Base {
        public int finished;
        public Child() { super(); finished = 9; }
        public Child(Factory factory) {
            this(factory.get(Leaf.class));
            finished = finished + 1;
        }
        public Child(Carrier<? extends Child> source) {
            super(source);
            finished = 9;
        }
    }
}
