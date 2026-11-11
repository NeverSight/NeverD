package fixture;

public class Trace {
    public static Trace hook;
    public static Object value;
    public static Provider returned;
    public static boolean accepted;
    public Trace() {}
    public void observe(int phase, Provider self, String key, Class token, Object predicate, Object result) {}
    public static void hit(int phase, Provider self, String key, Class token, Object predicate, Object result) {
        Trace current=hook;
        if(current!=null)current.observe(phase,self,key,token,predicate,result);
    }
}
