package fixture;
public class Bucket<E> extends java.util.ArrayList<E> {
    public Bucket() { Trace.hit(1);Trace.bucket=this; }
}
