package fixture;
public class Trace {
    public static Trace hook;
    public static int offset;
    public static Object overrideReply;
    public Trace() {}
    public void observe(int phase,Object self,Object first,Object second,Object values,int index,Object value) {}
    public static void hit(int phase,Object self,Object first,Object second,Object values,int index,Object value) {
        Trace current=hook;
        if(current!=null)current.observe(phase,self,first,second,values,index,value);
    }
}
