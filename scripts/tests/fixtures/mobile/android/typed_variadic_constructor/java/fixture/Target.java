package fixture;
public class Target<T> implements Marker<T> {
    public final boolean flag;
    public final Class<T> type;
    public final String[] names;
    public Target(Class<T> type,String... names) { this(false,type,names); }
    public Target(boolean flag,Class<T> type,String... names) {
        Trace.created++;Trace.hit(1);this.flag=flag;this.type=type;this.names=names;Trace.partial=this;
        Trace.action.run();Trace.hit(2);
    }
}
