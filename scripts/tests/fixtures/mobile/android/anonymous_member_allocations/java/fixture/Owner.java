package fixture;
public class Owner {
    public static final GenericBase<Owner, Float> PROPERTY =
        new GenericBase<Owner, Float>(Float.class, Names.first) {
            public Float get(Owner owner) { Trace.hit(5); return owner.value; }
            public void set(Owner owner, Float value) { Trace.hit(6); owner.value = value; }
        };
    static { Trace.hit(4); }
    public Float value;
}
