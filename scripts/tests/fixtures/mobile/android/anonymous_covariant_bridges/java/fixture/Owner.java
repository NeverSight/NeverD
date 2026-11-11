package fixture;
public class Owner implements Menu {
    public Class<?> witness;
    public Base create(Object token) {
        return new Base(token) {
            public Owner getItem() {
                Trace.hit(3);
                return Owner.this;
            }
        };
    }
}
