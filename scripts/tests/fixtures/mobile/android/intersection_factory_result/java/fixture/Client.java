package fixture;
public final class Client extends Factory implements First,Second {
    public int left,right;
    public Client() { super(); }
    public int first() { Trace.hit(6,this,left,0,null);return left; }
    public int second() { Trace.hit(7,this,0,right,null);return right; }
    public Object run() {
        Trace.hit(0,this,0,0,null);
        Factory result=Factory.obtain(this);
        Trace.hit(4,this,0,0,result);
        return (Client)result;
    }
    public void ignore() {
        Trace.hit(0,this,0,0,null);
        Factory.obtain(this);
        Trace.hit(4,this,0,0,null);
    }
}
