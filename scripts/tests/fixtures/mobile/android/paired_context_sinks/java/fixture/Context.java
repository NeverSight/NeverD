package fixture;

import java.util.Map;

public class Context {
    public Object keyReply,valueReply;
    public Context() {}
    public void callback(Map target,String key,Class first,Class second) {}
    @SuppressWarnings("unchecked")
    public <A,B> void fill(Map<? super A,? super B> target,String key,Class<A> first,Class<B> second) {
        Trace.hit(1,this,target,key,first,second,null,null);
        callback(target,key,first,second);
        A left=(A)keyReply;
        B right=(B)valueReply;
        Trace.hit(2,this,target,key,first,second,left,right);
        target.put(left,right);
        Trace.hit(5,this,target,key,first,second,left,right);
    }
}
