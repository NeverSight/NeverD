package fixture;
public final class Derived extends Producer {
    public int getCount() { Trace.hit(5); return super.getCount()+Trace.delta; }
}
