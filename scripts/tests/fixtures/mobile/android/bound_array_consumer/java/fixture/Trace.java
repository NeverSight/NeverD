package fixture;
public class Trace {
    public static Trace hook;
    public static Object next;
    public Trace() {}
    public void observe(int phase,Object receiver,Object array,Object value,int flags) {}
    public static void hit(int phase,Object receiver,Object array,Object value,int flags) {
        if (hook != null) hook.observe(phase,receiver,array,value,flags);
    }
}
