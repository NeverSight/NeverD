package fixture;

import java.util.List;

public class Context {
    public Object valueReply;
    public Context() {}
    public void callback(List target,String key,Class token) {}
    @SuppressWarnings("unchecked")
    public <T> void fill(List<? super T> target,String key,Class<T> token) {
        Trace.hit(1,this,target,key,token,null);
        callback(target,key,token);
        T value=(T)valueReply;
        Trace.hit(2,this,target,key,token,value);
        target.set(0,value);
        Trace.hit(5,this,target,key,token,value);
    }
}
