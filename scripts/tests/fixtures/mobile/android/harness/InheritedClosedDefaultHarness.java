import fixture.*;
import kotlin.jvm.functions.Function1;
import java.lang.reflect.*;
public final class InheritedClosedDefaultHarness {
    static int mode;
    static Object expectedReceiver, expectedPredicate;
    static Throwable failure;
    static void require(boolean ok) { if (!ok) throw new AssertionError("owned inherited closed default"); }
    static final class Probe implements Function1<Parent,Boolean> {
        public Boolean invoke(Parent value) { return observe(value); }
        public String toString() { throw new AssertionError("owned callback operand effect"); }
        public int hashCode() { throw new AssertionError("owned callback operand effect"); }
    }
    public static Boolean observe(Parent value) {
        require(value == expectedReceiver && Trace.receiver == expectedReceiver && Trace.predicate == expectedPredicate);
        require(Trace.enters == 1 && Trace.events == 12 && Trace.virtuals == 0 && Trace.callbacks == 0);
        Trace.callbacks++; Trace.events = 123;
        if (mode == 3) throw (RuntimeException)failure;
        if (mode == 4) throw (Error)failure;
        return mode == 2 ? null : mode != 1;
    }
    static void metadata(String accessor,String forward,boolean projected) throws Exception {
        TypeVariable<?>[] own = Child.class.getTypeParameters();
        require(own.length == 1 && own[0].getGenericDeclaration() == Child.class && own[0].getBounds().length == 1 && own[0].getBounds()[0] == Object.class);
        Method raw = Child.class.getDeclaredMethod(accessor,Child.class,Function1.class);
        require(raw.getReturnType() == boolean.class && raw.getTypeParameters().length == 0 && raw.isSynthetic() == !projected);
        require(raw.getGenericParameterTypes()[0] == Child.class && raw.getGenericParameterTypes()[1] == Function1.class);
        Method parent = Parent.class.getDeclaredMethod("matches",Function1.class);
        require(parent.isDefault() && parent.getReturnType() == boolean.class && parent.getTypeParameters().length == 0);
        ParameterizedType callback = (ParameterizedType)parent.getGenericParameterTypes()[0];
        require(callback.getRawType() == Function1.class && callback.getActualTypeArguments()[1] == Boolean.class);
        WildcardType argument = (WildcardType)callback.getActualTypeArguments()[0];
        require(argument.getLowerBounds().length == 1 && argument.getLowerBounds()[0] == Parent.class && argument.getUpperBounds()[0] == Object.class);
        Method entry = Class.forName("fixture.Child$DefaultImpls").getDeclaredMethod(forward,Child.class,Function1.class);
        require(entry.getReturnType() == boolean.class && entry.getTypeParameters().length == 1);
        TypeVariable<?> formal = entry.getTypeParameters()[0];
        require(formal.getGenericDeclaration().equals(entry) && formal.getBounds()[0] == Object.class);
        ParameterizedType receiver = (ParameterizedType)entry.getGenericParameterTypes()[0];
        require(receiver.getRawType() == Child.class && receiver.getActualTypeArguments()[0].equals(formal));
        require(entry.getGenericParameterTypes()[1].equals(callback));
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]), alias=Integer.parseInt(args[1]);mode=Integer.parseInt(args[2]);
        metadata(args[3],args[4],Boolean.parseBoolean(args[5]));
        for(int attempt=0;attempt<2;attempt++) {
            Trace.enters=Trace.callbacks=Trace.virtuals=Trace.events=Trace.fail=0;
            Trace.receiver=Trace.predicate=null;
            failure=mode==4?new AssertionError("owned callback error"):new IllegalArgumentException("owned exact failure");
            Trace.failure=(failure instanceof RuntimeException)?(RuntimeException)failure:null;
            InheritedClosedDefaultCaller.Receiver receiver=alias==0?new InheritedClosedDefaultCaller.Receiver():new InheritedClosedDefaultCaller.Derived();
            Function1<Parent,Boolean> callback=alias==0?new Probe():receiver;
            if(mode==5)callback=null;if(mode==6)receiver=null;
            expectedReceiver=receiver;expectedPredicate=callback;
            Trace.fail=mode==7?1:mode==8?2:0;
            Throwable seen=null;boolean value=false;
            try {value=InheritedClosedDefaultCaller.call(route,receiver,callback,args[3],args[4]);}catch(Throwable error){seen=error;}
            if(mode==0 || mode==1)require(seen==null && value==(mode==0));
            else if(mode==2 || mode==5 || mode==6)require(seen!=null && seen.getClass()==NullPointerException.class);
            else require(seen==failure);
            int enters=mode==6?0:1,callbacks=(mode<=4)?1:0,events=mode==6?0:mode==7?1:(mode<=4?123:12);
            require(Trace.enters==enters && Trace.callbacks==callbacks && Trace.events==events && Trace.virtuals==0);
            require(Trace.receiver==expectedReceiver && Trace.predicate==(mode==7 || mode==6?null:expectedPredicate));
        }
        System.out.println("attempts=2,inherited-closed-default=matched");
    }
}
