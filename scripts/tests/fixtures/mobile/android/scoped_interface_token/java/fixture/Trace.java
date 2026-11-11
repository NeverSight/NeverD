package fixture;
public final class Trace {
    private Trace() {}
    public static Context current;
    public static Observer observer;
    public interface Observer { void observe(int phase,Factory receiver,String name,Class token,Context context,Bound result); }
    public static void hit(int phase,Factory receiver,String name,Class token,Context context,Bound result) { if(observer!=null)observer.observe(phase,receiver,name,token,context,result); }
}
