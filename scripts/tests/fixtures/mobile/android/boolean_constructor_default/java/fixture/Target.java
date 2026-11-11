package fixture;
public class Target<K,V> {
    public final boolean flag;
    public final Callback<V,K> mapper;
    public Target(Callback<V,K> mapper) { this(false,mapper); }
    public Target(Callback<V,K> mapper,byte marker) { this(true,mapper); }
    public Target(boolean flag,Callback<V,K> mapper) {
        Trace.hit(1);Trace.partial=this;this.flag=flag;
        Trace.hit(2);this.mapper=mapper;Trace.hit(3);
    }
    public Target(int marker,Callback<V,K> mapper) {
        Trace.hit(9);Trace.partial=this;this.flag=false;this.mapper=mapper;Trace.hit(8);
    }
}
