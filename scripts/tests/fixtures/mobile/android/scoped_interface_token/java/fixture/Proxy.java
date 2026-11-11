package fixture;
public abstract class Proxy<T> extends Factory<T> implements Entry {
    protected Proxy() {}
    private final <T extends java.lang.annotation.Annotation> T adapt(String name,Class<T> token) {
        Context context=Trace.current;
        Trace.hit(1,this,name,token,context,null);
        Bound result=(Bound)create(name,token,context);
        Trace.hit(2,this,name,token,context,result);
        return (T)result;
    }
    public final String run(Object name,Object token) {
        String n=(String)name;Class t=(Class)token;
        Trace.hit(0,this,n,t,null,null);
        Bound result=(Bound)adapt(n,t);
        Trace.hit(3,this,n,t,null,result);return (String)result.value;
    }
    public final void ignore(Object name,Object token) {
        String n=(String)name;Class t=(Class)token;
        Trace.hit(0,this,n,t,null,null);
        adapt(n,t);
        Trace.hit(3,this,n,t,null,null);
    }
}
