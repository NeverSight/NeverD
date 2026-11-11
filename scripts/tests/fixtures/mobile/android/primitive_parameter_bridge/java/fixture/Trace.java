package fixture;
public class Trace {
    public static Trace hook;
    public Trace() {}
    public void observe(int phase,Object value,float number) {}
    public static void hit(int phase,Object value,float number) {
        Trace current=hook;
        if(current!=null)current.observe(phase,value,number);
    }
}
