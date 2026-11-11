package fixture;

public class Trace {
    public static Trace hook;
    public static Object reply;
    public Trace() {}
    public void observe(int phase,Object owner,Object pair,Object single,Object seed,Object context,Object value,int direction,long stamp,boolean forward,boolean wrap) {}
    public static void hit(int phase,Object owner,Object pair,Object single,Object seed,Object context,Object value,int direction,long stamp,boolean forward,boolean wrap) {
        Trace current=hook;
        if(current!=null)current.observe(phase,owner,pair,single,seed,context,value,direction,stamp,forward,wrap);
    }
}
