package fixture;
public abstract class Proxy extends Factory {
    protected Proxy() {}
    private final <T extends Bound> T adapt(String name,Token<T> token) {
        Context context=Trace.current;
        Trace.hit(1,this,name,token,context,null);
        Bound result=create(name,token,context);
        Trace.hit(2,this,name,token,context,result);
        return (T)result;
    }
    public final String run(Object name,Object token) {
        String n=(String)name;Token t=(Token)token;
        Trace.hit(0,this,n,t,null,null);
        Bound result=adapt(n,t);
        Trace.hit(3,this,n,t,null,result);return (String)result.value;
    }
    public final void ignore(Object name,Object token) {
        String n=(String)name;Token t=(Token)token;
        Trace.hit(0,this,n,t,null,null);
        adapt(n,t);
        Trace.hit(3,this,n,t,null,null);
    }
}
