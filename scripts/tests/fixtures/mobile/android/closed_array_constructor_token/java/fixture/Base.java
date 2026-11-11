package fixture;
public class Base<T> {
    static { Trace.hit(1); }
    public int left, padding, right;
    public Class<T> token;
    public Base(int left, Class<T> token, int right) {
        this.left=left;
        this.token=token;
        this.padding=0;
        this.right=right;
        Trace.parent=this;
        Trace.hit(2);
        Trace.hit(3);
    }
}
