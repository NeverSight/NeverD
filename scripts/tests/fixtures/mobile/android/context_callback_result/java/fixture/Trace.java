package fixture;
public class Trace {
    public static Trace hook;
    public Trace() {}
    public void observe(int phase,Object owner,CharSequence context,Object callback,int left,int right,int limit,boolean flag,Object result) {}
    public static void hit(int phase,Object owner,CharSequence context,Object callback,int left,int right,int limit,boolean flag,Object result) {
        Trace current=hook;
        if(current!=null)current.observe(phase,owner,context,callback,left,right,limit,flag,result);
    }
}
