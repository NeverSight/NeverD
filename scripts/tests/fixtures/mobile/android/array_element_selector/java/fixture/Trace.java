package fixture;

public class Trace {
    public static Trace hook;
    public static Object array;
    public static int mode,index,rank,delta;
    public static boolean flag,want,override;
    public static Object reply;
    public Trace() {}
    public void observe(int phase,Object values,int mask,Object rule,Object value) {}
    public static void hit(int phase,Object values,int mask,Object rule,Object value) {
        Trace current=hook;
        if(current!=null)current.observe(phase,values,mask,rule,value);
    }
}
