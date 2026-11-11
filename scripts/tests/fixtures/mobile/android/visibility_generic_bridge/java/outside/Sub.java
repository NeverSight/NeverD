package outside;
import fixture.Core;
import fixture.Trace;
public class Sub extends Core {
    @Override public Object read(Object first,Object second,java.util.List<String[]> values,int index) {
        Trace.hit(5,this,first,second,values,index,null);
        return Trace.overrideReply;
    }
    @SuppressWarnings("unchecked")
    public Object parent(Object first,Object second,java.util.List values,int index) {
        return super.read(first,second,values,index);
    }
}
