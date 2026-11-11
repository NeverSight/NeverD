package fixture;

public final class Child extends Receiver<Object,String> {
    public Child() {}
    public Leaf<Object> getItems() {
        Trace.events = Trace.events * 10 + 9;
        return super.getItems();
    }
}
