package fixture;
public class Trace {
    public static Trace hook;
    public Trace() {}
    public void observe(int phase,Object owner,Object activity,Object context,Object handler,int code) {}
    public static void hit(int phase,Object owner,Object activity,Object context,Object handler,int code) {
        Trace current=hook;
        if(current!=null)current.observe(phase,owner,activity,context,handler,code);
    }
    public static Object require(Object value,int phase) {
        hit(phase,null,value,null,null,0);
        if(value==null)throw new NullPointerException("owned-required");
        return value;
    }
}
