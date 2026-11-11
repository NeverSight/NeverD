package fixture;
public class Trace {
    public static Trace hook;
    public Trace() {}
    public void observe(int phase,Object owner,Base input,int code) {}
    public static void hit(int phase,Object owner,Base input,int code) {
        Trace current=hook;
        if(current!=null)current.observe(phase,owner,input,code);
    }
    public static int choose(int value) { hit(1,null,null,value);return value; }
}
