package fixture;
public class Trace {
    public static Trace hook;
    public static Object next;
    public Trace() {}
    public void observe(int phase,Object receiver,Object left,Object right) {}
    public static void hit(int phase,Object receiver,Object left,Object right) {
        if (hook != null) hook.observe(phase,receiver,left,right);
    }
}
