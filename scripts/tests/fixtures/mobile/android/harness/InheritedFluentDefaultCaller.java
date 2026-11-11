import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class InheritedFluentDefaultCaller {
    public static class Receiver implements Child<String> {
        public Parent combine(Parent other){Trace.virtuals++;return other;}
        public Parent control(Parent other){return Child.super.combine(other);}
        public String toString(){throw new AssertionError("owned receiver effect");}
        public int hashCode(){throw new AssertionError("owned receiver effect");}
    }
    public static final class Derived extends Receiver {
        public Parent combine(Parent other){Trace.virtuals++;return null;}
    }
    public static Parent call(int route,Child receiver,Parent other,String accessor,String forward)throws Throwable{
        try{
            if(route==3)return ((Receiver)receiver).control(other);
            Class<?> owner=route==2?Class.forName("fixture.Child$DefaultImpls"):Child.class;String name=route==2?forward:accessor;
            if(route!=1)return (Parent)owner.getMethod(name,Child.class,Parent.class).invoke(null,receiver,other);
            MethodHandle handle=MethodHandles.publicLookup().findStatic(owner,name,MethodType.methodType(Parent.class,Child.class,Parent.class));
            return (Parent)handle.invokeExact(receiver,other);
        }catch(InvocationTargetException error){throw error.getCause();}
    }
}
