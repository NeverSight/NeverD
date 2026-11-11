package fixture;
public class Target {
    public final boolean flag;
    public final Callback<? extends CharSequence,?> mapper;
    public Target(Callback<? extends CharSequence,?> mapper) { this(false,mapper); }
    public Target(Callback<? extends CharSequence,?> mapper,byte marker) { this(true,mapper); }
    public Target(boolean flag,Callback<? extends CharSequence,?> mapper) {
        Trace.hit(1);Trace.partial=this;this.flag=flag;
        Trace.hit(2);this.mapper=mapper;Trace.hit(3);
    }
    public Target(int marker,Callback<? extends CharSequence,?> mapper) {
        Trace.hit(9);Trace.partial=this;this.flag=false;this.mapper=mapper;Trace.hit(8);
    }
}
