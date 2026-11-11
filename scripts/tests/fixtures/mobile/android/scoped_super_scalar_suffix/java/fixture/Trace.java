package fixture;
public class Trace {
    public static Trace hook;
    public Trace() {}
    public void observe(int phase,Object parent,Object seed,Object property,Object result) {}
    public static void hit(int phase,Object parent,Object seed,Object property,Object result) {
        Trace current=hook;
        if(current!=null)current.observe(phase,parent,seed,property,result);
    }
}
