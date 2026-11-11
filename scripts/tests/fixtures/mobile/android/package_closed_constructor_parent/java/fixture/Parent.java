package fixture;

public abstract class Parent {
    public Carrier<String, String> first;
    public Carrier<String, String> second;
    public Carrier<String, Class> third;
    protected Parent(Carrier<String, String> first, Carrier<String, String> second, Carrier<String, Class> third) {
        Trace.hit(5, this, first, 0);
        this.first = first;
        Trace.hit(6, this, second, 0);
        this.second = second;
        Trace.hit(7, this, third, 0);
        this.third = third;
        Trace.hit(8, this, null, 0);
    }
}
