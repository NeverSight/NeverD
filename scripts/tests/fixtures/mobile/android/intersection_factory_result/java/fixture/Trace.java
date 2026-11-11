package fixture;
public class Trace {
    public static Trace hook;
    public Trace() {}
    public void observe(int phase,Object owner,int left,int right,Object result) {}
    public static void hit(int phase,Object owner,int left,int right,Object result) {
        Trace current=hook;
        if(current!=null)current.observe(phase,owner,left,right,result);
    }
}
