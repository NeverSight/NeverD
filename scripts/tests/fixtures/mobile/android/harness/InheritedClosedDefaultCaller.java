import fixture.*;
import kotlin.jvm.functions.Function1;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class InheritedClosedDefaultCaller {
    public static class Receiver implements Child<String>, Function1<Parent,Boolean> {
        public boolean matches(Function1<? super Parent,Boolean> callback) { Trace.virtuals++; return false; }
        public Boolean invoke(Parent value) { return InheritedClosedDefaultHarness.observe(value); }
        public boolean control(Function1<? super Parent,Boolean> callback) { return Child.super.matches(callback); }
        public String toString() { throw new AssertionError("owned receiver operand effect"); }
        public int hashCode() { throw new AssertionError("owned receiver operand effect"); }
    }
    public static final class Derived extends Receiver {
        public boolean matches(Function1<? super Parent,Boolean> callback) { Trace.virtuals++; return true; }
    }
    public static boolean call(int route, Child receiver, Function1 callback, String accessor, String forward) throws Throwable {
        try {
            if (route == 5) return ((Receiver)receiver).control(callback);
            Class<?> owner = route < 3 ? Child.class : Class.forName("fixture.Child$DefaultImpls");
            String name = route < 3 ? accessor : forward;
            if (route == 0 || route == 3) return (Boolean)owner.getMethod(name, Child.class, Function1.class).invoke(null,receiver,callback);
            MethodType type = MethodType.methodType(boolean.class,Child.class,Function1.class);
            MethodHandle handle = route == 2 ? MethodHandles.publicLookup().unreflect(owner.getMethod(name,Child.class,Function1.class)) : MethodHandles.publicLookup().findStatic(owner,name,type);
            return (boolean)handle.invokeExact(receiver,callback);
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
}
