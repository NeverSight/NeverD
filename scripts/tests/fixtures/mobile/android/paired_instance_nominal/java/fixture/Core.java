package fixture;
import java.util.function.BiFunction;
public final class Core {
    private Core() {}
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Object run(Object owner,Object first,Object second,Object callback) {
        Provider self=(Provider)owner;
        Class left=(Class)first;
        Class right=(Class)second;
        BiFunction function=(BiFunction)callback;
        Trace.hit(3,self,left,right,function,null);
        Object result=self.bind(left,right,function);
        Trace.hit(4,self,left,right,function,result);
        return result;
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static void ignore(Object owner,Object first,Object second,Object callback) {
        Provider self=(Provider)owner;
        Class left=(Class)first;
        Class right=(Class)second;
        BiFunction function=(BiFunction)callback;
        Trace.hit(3,self,left,right,function,null);
        self.bind(left,right,function);
        Trace.hit(4,self,left,right,function,null);
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static void consume(Object owner,Object first,Object second,Object callback) {
        Provider self=(Provider)owner;
        Class left=(Class)first;
        Class right=(Class)second;
        BiFunction function=(BiFunction)callback;
        Trace.hit(3,self,left,right,function,null);
        self.consume(left,right,function);
        Trace.hit(4,self,left,right,function,null);
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static long measure(Object owner,Object first,Object second,Object callback) {
        Provider self=(Provider)owner;
        Class left=(Class)first;
        Class right=(Class)second;
        BiFunction function=(BiFunction)callback;
        Trace.hit(3,self,left,right,function,null);
        long result=self.measure(left,right,function);
        Trace.hit(4,self,left,right,function,null);
        return result;
    }
}
