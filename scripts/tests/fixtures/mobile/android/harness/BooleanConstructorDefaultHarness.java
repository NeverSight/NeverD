import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class BooleanConstructorDefaultHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned boolean constructor default"); }
    static final class Fn implements Callback {
        int calls;Object received;
        public Object apply(Object value) { Trace.hit(4);calls++;received=value;return Trace.result; }
    }
    static Target create(int route,int entry,Callback fn) throws Throwable {
        if(route==0) {
            if(entry==0)return new Target(fn);
            if(entry==1)return new Target(fn,(byte)7);
            if(entry==2)return new Target(false,fn);
            if(entry==3)return new Target(true,fn);
            return new Target(7,fn);
        }
        if(route==1)return BooleanConstructorDefaultCaller.create(entry,fn);
        if(route==2) {
            try {
                if(entry==0)return (Target)Target.class.getConstructor(Callback.class).newInstance(fn);
                if(entry==1)return (Target)Target.class.getConstructor(Callback.class,byte.class).newInstance(fn,(byte)7);
                if(entry<4)return (Target)Target.class.getConstructor(boolean.class,Callback.class).newInstance(entry==3,fn);
                return (Target)Target.class.getConstructor(int.class,Callback.class).newInstance(7,fn);
            } catch(InvocationTargetException error) { throw error.getCause(); }
        }
        MethodHandles.Lookup lookup=MethodHandles.publicLookup();
        if(entry==0)return (Target)lookup.findConstructor(Target.class,MethodType.methodType(void.class,Callback.class)).invokeExact(fn);
        if(entry==1)return (Target)lookup.findConstructor(Target.class,MethodType.methodType(void.class,Callback.class,byte.class)).invokeExact(fn,(byte)7);
        if(entry<4)return (Target)lookup.findConstructor(Target.class,MethodType.methodType(void.class,boolean.class,Callback.class)).invokeExact(entry==3,fn);
        return (Target)lookup.findConstructor(Target.class,MethodType.methodType(void.class,int.class,Callback.class)).invokeExact(7,fn);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),entry=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        Trace.failAt=mode>=1&&mode<=4?mode:0;Trace.failure=new RuntimeException("owned boolean failure");
        Fn fn=new Fn();Callback mapper=mode==5?null:fn;Object input=new String("owned input");
        Trace.result=mode==6?new Object():mode==7?null:new String("owned result");
        Object sentinel=new Object();boolean expectedFlag=entry==1||entry==3;
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.partial=sentinel;int calls=fn.calls;Object received=fn.received;
            Target target=null;Object actual=null;Throwable failure=null;
            try { target=create(route,entry,mapper);actual=BooleanConstructorDefaultCaller.apply(target,(String)input); }
            catch(Throwable error) { failure=error; }
            boolean integer=entry==4,constructed=integer||mode!=1&&mode!=2&&mode!=3;
            long expected=integer?98:mode==1?1:mode==2?12:123;
            if(constructed&&mapper!=null)expected=expected*10+4;
            require(Trace.events==expected);
            boolean partial=integer||mode!=1;require((Trace.partial!=sentinel)==partial);
            if(partial) { Target value=(Target)Trace.partial;require(value.flag==expectedFlag&&value.mapper==(!integer&&mode==2?null:mapper)); }
            if(constructed)require(target==Trace.partial&&target.flag==expectedFlag&&target.mapper==mapper);
            boolean called=constructed&&mapper!=null&&mode!=4;
            require(fn.calls==calls+(called?1:0)&&fn.received==(called?input:received));
            if(!integer&&mode>=1&&mode<=3||constructed&&mode==4)require(failure==Trace.failure);
            else if(constructed&&mode==5)require(failure instanceof NullPointerException);
            else if(constructed&&mode==6)require(failure instanceof ClassCastException);
            else require(failure==null&&actual==Trace.result);
            System.out.println("route="+route+",operation="+entry+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
