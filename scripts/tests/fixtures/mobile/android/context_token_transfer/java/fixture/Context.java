package fixture;

import java.util.List;

public class Context {
    public Object reply,valueReply;
    public Context() {}
    public void callback(List target,String key,Class token) {}
    @SuppressWarnings("unchecked")
    public <T> List<T> read(List<T> target,String key,Class<? extends T> token) {
        Trace.hit(1,this,target,key,token,null,null);
        callback(target,key,token);
        T value=(T)valueReply;
        List<T> result=(List<T>)reply;
        Trace.hit(2,this,target,key,token,result,value);
        target.set(0,value);
        Trace.hit(5,this,target,key,token,result,value);
        return result;
    }
}
