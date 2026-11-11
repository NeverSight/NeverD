package fixture;

public class Client {
    public Client() {}
    public static String run(Object values,int mode,Object rule) {
        Trace.hit(6,values,mode,rule,null);
        Object result=Core.choose((Object[])values,mode,(Core.Selector)rule);
        Trace.hit(7,values,mode,rule,result);
        return (String)result;
    }
    public static void ignore(Object values,int mode,Object rule) {
        Trace.hit(6,values,mode,rule,null);
        Core.choose((Object[])values,mode,(Core.Selector)rule);
        Trace.hit(7,values,mode,rule,null);
    }
}
