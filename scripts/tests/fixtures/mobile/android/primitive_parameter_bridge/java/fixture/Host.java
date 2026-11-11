package fixture;
public final class Host {
    private Host() {}
    private static final Middle INSTANCE=new Middle("owned-name") {
        public float get(Value value) {
            Trace.hit(1,value,0.0f);
            float saved=value.number;
            Trace.hit(3,value,saved);
            return saved;
        }
        public void put(Value value,float number) {
            Trace.hit(2,value,number);
            value.number=number;
            Trace.hit(3,value,number);
        }
    };
    public static Middle shared() { return INSTANCE; }
}
