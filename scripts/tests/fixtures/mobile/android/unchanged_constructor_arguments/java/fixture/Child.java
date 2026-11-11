package fixture;
public final class Child extends Base {
    public int value;
    public Child(int initial) { super(null,0);this.value=initial; }
    public Child(Child other,boolean branch) {
        super(other,branch ? Trace.choose(17) : Trace.choose(29));
        Trace.hit(4,this,other,code);
        this.value=other.value;
        Trace.hit(5,this,other,code);
    }
}
