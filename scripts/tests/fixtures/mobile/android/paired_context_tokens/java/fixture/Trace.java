package fixture;

public class Trace {
    public static Trace hook;
    public Trace() {}
    public void observe(int phase,Context context,String key,Class first,Class second,Object result) {}
    public static void hit(int phase,Context context,String key,Class first,Class second,Object result) {
        Trace current=hook;
        if(current!=null)current.observe(phase,context,key,first,second,result);
    }
}
