package fixture;
public class Trace {
    public static Trace hook;
    public Trace() {}
    public void observe(int phase,Object owner,Object input,Object context,Object callback,Object result) {}
    public static void hit(int phase,Object owner,Object input,Object context,Object callback,Object result) {
        Trace current=hook;
        if(current!=null)current.observe(phase,owner,input,context,callback,result);
    }
}
