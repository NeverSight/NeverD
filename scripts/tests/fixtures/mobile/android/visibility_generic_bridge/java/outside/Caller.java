package outside;
import fixture.Core;
import fixture.Trace;
public final class Caller {
    private Caller() {}
    @SuppressWarnings("unchecked")
    public static Object run(Object owner,Object first,Object second,Object input,int index) {
        Core self=(Core)owner;
        java.util.List values=(java.util.List)input;
        Trace.hit(4,self,first,second,values,index,null);
        Object result=self.read(first,second,values,index);
        Trace.hit(6,self,first,second,values,index,result);
        return result;
    }
    @SuppressWarnings("unchecked")
    public static void ignore(Object owner,Object first,Object second,Object input,int index) {
        Core self=(Core)owner;
        java.util.List values=(java.util.List)input;
        Trace.hit(4,self,first,second,values,index,null);
        self.read(first,second,values,index);
        Trace.hit(6,self,first,second,values,index,null);
    }
}
