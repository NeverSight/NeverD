package fixture;
public class Trace {
    public static Trace hook;
    public Trace() {}
    public void observe(int phase,Object context,Object token,Object first,Object second,Object third,Object result) {}
    public static void hit(int phase,Object context,Object token,Object first,Object second,Object third,Object result) {
        Trace current=hook;
        if(current!=null)current.observe(phase,context,token,first,second,third,result);
    }
}
