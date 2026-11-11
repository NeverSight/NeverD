package fixture;
public class Owner {
    public static int events;
    public static Owner current;
    public static Base escaped;
    public static RuntimeException failure;
    public int stage;
    public Child child;
    public Owner(Callback callback) {
        events = events * 5 + 3;
        current = this;
        stage = 1;
        child = new Child(callback);
        stage = 2;
        events = events * 5 + 4;
    }
    public static Owner create(Callback callback) { return new Owner(callback); }
    public class Child extends Base<Object> {
        public Child(Callback callback) {
            super((Callback<Object>) callback);
            events = events * 5 + 5;
        }
        public Owner owner() { return Owner.this; }
    }
}
