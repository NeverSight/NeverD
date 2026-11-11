package fixture;
public class Trace {
    public static Trace hook;
    public static Object firstValue,secondValue,returned;
    public static Boolean accepted;
    public static long number;
    public Trace() {}
    public void observe(int phase,Provider self,Class first,Class second,Object callback,Object result) {}
    public static void hit(int phase,Provider self,Class first,Class second,Object callback,Object result) {
        Trace current=hook;
        if(current!=null)current.observe(phase,self,first,second,callback,result);
    }
}
