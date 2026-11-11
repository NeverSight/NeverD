package fixture;

import java.util.HashMap;

public class Context {
    public Object reply;
    public Context() {}
    public void callback(String key,Class first,Class second) {}
    @SuppressWarnings("unchecked")
    public <A,B> HashMap<A,B> read(String key,Class<? extends A> first,Class<? extends B> second) {
        Trace.hit(1,this,key,first,second,null);
        callback(key,first,second);
        HashMap<A,B> result=(HashMap<A,B>)reply;
        Trace.hit(2,this,key,first,second,result);
        return result;
    }
}
