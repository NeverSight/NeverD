import fixture.Client;
import fixture.Provider;
import fixture.Failure;
import fixture.Trace;
import java.lang.reflect.Type;
import java.lang.reflect.InvocationTargetException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
public final class InstanceFormalContextHarness {
    private static void require(boolean value) {if(!value)throw new AssertionError("owned instance formal context");}
    private static final class TrapType implements Type {
        public String getTypeName(){throw new AssertionError("owned unexpected type inspection");}
        public String toString(){throw new AssertionError("owned unexpected text inspection");}
        public boolean equals(Object value){throw new AssertionError("owned unexpected equality");}
        public int hashCode(){throw new AssertionError("owned unexpected hash");}
    }
    @SuppressWarnings("unchecked")
    private static <E> E override(Provider self,Type type,Object value,E fallback) {
        Trace.calls++;Trace.receiver=self;Trace.type=type;Trace.value=value;Trace.fallback=fallback;Trace.hit(7);
        Trace.action.run();Trace.hit(2);return (E)Trace.result;
    }
    private static Object call(int route,int kind,Type type,Object value,Object fallback) throws Throwable {
        boolean soft=kind==2;
        if(route==0)return kind==0?Client.<Object>call(type,value,fallback):Client.<Object>call(type,value,fallback,soft);
        if(route==1)return kind==0?InstanceFormalContextCaller.defaults(type,value,fallback):InstanceFormalContextCaller.explicit(type,value,fallback,soft);
        if(route==2){try{return kind==0?Client.class.getMethod("call",Type.class,Object.class,Object.class).invoke(null,type,value,fallback):Client.class.getMethod("call",Type.class,Object.class,Object.class,boolean.class).invoke(null,type,value,fallback,soft);}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(kind==0)return (Object)MethodHandles.publicLookup().findStatic(Client.class,"call",MethodType.methodType(Object.class,Type.class,Object.class,Object.class)).invokeExact(type,value,fallback);
        return (Object)MethodHandles.publicLookup().findStatic(Client.class,"call",MethodType.methodType(Object.class,Type.class,Object.class,Object.class,boolean.class)).invokeExact(type,value,fallback,soft);
    }
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),kind=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);final boolean soft=kind==2;
        final Type type=mode==1?null:new TrapType();final String fallback=mode==9?null:new String("owned fallback");final Object value=mode==2?fallback:new Object(),payload=new Object();
        final Object desired=mode==7?new Object():mode==8?null:new String("owned opaque result");
        final Failure actionFailure=new Failure("owned action failure");final Error actionError=new Error("owned action error");
        final Provider provider=mode==10?null:mode==11?new Provider(){public <E>E convert(Type type,Object value,E fallback)throws Failure{return override(this,type,value,fallback);}}:new Provider();
        Trace.provider=provider;Trace.failure=new Failure("owned stage failure");Trace.failAt=mode==5?1:mode==6?2:0;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object();Trace.events=0;Trace.receiver=Trace.type=Trace.value=Trace.fallback=Trace.result=Trace.payload=sentinel;int calls=Trace.calls;
            Trace.action=new Runnable(){public void run(){
                require(Trace.events==(mode==11?7:1)&&Trace.receiver==provider&&Trace.type==type&&Trace.value==value&&Trace.fallback==fallback&&Trace.payload==sentinel);
                Trace.hit(3);Trace.payload=payload;Trace.result=desired;if(mode==3||mode==9)throw actionFailure;if(mode==4)throw actionError;
            }};
            Object actual=sentinel;int length=42;Throwable failure=null;try{actual=call(route,kind,type,value,fallback);length=((String)actual).length();}catch(Throwable error){failure=error;}
            long events=mode==10?0:mode==5?1:mode==3||mode==4||mode==9?13:mode==11?732:132;
            require(Trace.events==events&&Trace.calls==calls+(mode==10?0:1));
            boolean caught=mode==3||mode==5||mode==6||mode==9||mode==10;
            Throwable expected=mode==3||mode==9?actionFailure:mode==4?actionError:mode==5||mode==6?Trace.failure:null;
            if(caught&&soft) {
                require(actual==fallback);if(fallback==null)require(failure instanceof NullPointerException&&length==42);else require(failure==null&&length==fallback.length());
            } else if(expected!=null)require(failure==expected&&actual==sentinel&&length==42);
            else if(mode==10)require(failure instanceof NullPointerException&&actual==sentinel&&length==42);
            else if(mode==7)require(failure instanceof ClassCastException&&actual==desired&&length==42);
            else if(mode==8)require(failure instanceof NullPointerException&&actual==desired&&length==42);
            else require(failure==null&&actual==desired&&length==((String)desired).length());
            if(mode==10)require(Trace.receiver==sentinel&&Trace.type==sentinel&&Trace.value==sentinel&&Trace.fallback==sentinel);
            else require(Trace.receiver==provider&&Trace.type==type&&Trace.value==value&&Trace.fallback==fallback);
            require(Trace.payload==(mode==5||mode==10?sentinel:payload));
            System.out.println("route="+route+",operation="+kind+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
