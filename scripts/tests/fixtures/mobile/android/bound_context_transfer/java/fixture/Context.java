package fixture;

import java.util.List;

public class Context {
    public Object reply,valueReply;
    public Context() {}
    public void callback(List target,String key) {}
    @SuppressWarnings("unchecked")
    public <T extends Bound> List<T> read(List<T> target,String key) {
        Trace.hit(1,this,target,key,null,null);
        callback(target,key);
        T value=(T)valueReply;
        List<T> result=(List<T>)reply;
        Trace.hit(2,this,target,key,result,value);
        target.set(0,value);
        Trace.hit(5,this,target,key,result,value);
        return result;
    }
}
