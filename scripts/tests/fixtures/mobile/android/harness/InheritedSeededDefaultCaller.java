import fixture.*;
import kotlin.jvm.functions.Function2;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class InheritedSeededDefaultCaller {
    public static class Receiver implements Child<String>,Function2<Object,Object,Object> {
        public <R> R foldIn(R seed,Function2<? super R,? super Parent,? extends R> operation) {Trace.virtuals++;return seed;}
        public <R> R foldOut(R seed,Function2<? super Parent,? super R,? extends R> operation) {Trace.virtuals++;return seed;}
        public Object invoke(Object a,Object b) {return InheritedSeededDefaultHarness.observe(a,b);}
        public Object control(int direction,Object seed,Function2 operation) {return direction==0?Child.super.foldIn(seed,operation):Child.super.foldOut(seed,operation);}
        public String toString() {throw new AssertionError("owned receiver effect");}
        public int hashCode() {throw new AssertionError("owned receiver effect");}
    }
    public static final class Derived extends Receiver {
        public <R> R foldIn(R seed,Function2<? super R,? super Parent,? extends R> operation) {Trace.virtuals++;return null;}
        public <R> R foldOut(R seed,Function2<? super Parent,? super R,? extends R> operation) {Trace.virtuals++;return null;}
    }
    public static Object call(int route,int direction,Child receiver,Object seed,Function2 operation,String accessor,String forward) throws Throwable {
        try {
            if(route==3)return ((Receiver)receiver).control(direction,seed,operation);
            Class<?> owner=route==2?Class.forName("fixture.Child$DefaultImpls"):Child.class;
            String name=route==2?forward:accessor;
            if(route!=1)return owner.getMethod(name,Child.class,Object.class,Function2.class).invoke(null,receiver,seed,operation);
            MethodHandle handle=MethodHandles.publicLookup().findStatic(owner,name,MethodType.methodType(Object.class,Child.class,Object.class,Function2.class));
            return (Object)handle.invokeExact(receiver,seed,operation);
        }catch(InvocationTargetException error){throw error.getCause();}
    }
}
