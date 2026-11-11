import fixture.Client;
import fixture.Trace;
import java.util.Map;
import java.util.HashMap;
import java.util.TreeMap;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class PairedRawTokenContextHarness {
    private static void require(boolean value){if(!value)throw new AssertionError("owned paired raw token context");}
    private static final class FutureMap extends HashMap<Object,Object> {
        public Object get(Object key){require(Trace.events==132);Trace.hit(4);Trace.getAction.run();return Trace.read;}
        public int size(){throw new AssertionError("owned unexpected size");}
        public java.util.Set<Map.Entry<Object,Object>> entrySet(){throw new AssertionError("owned unexpected entries");}
    }
    private static Map<?,?> call(int route,int kind,Class<? extends Map> type,Class<String> keys,Class<Integer> values,Object value)throws Throwable {
        if(route==0)return kind==0?Client.defaults(keys,values,value):Client.explicit(type,keys,values,value);
        if(route==1)return kind==0?PairedRawTokenContextCaller.defaults(keys,values,value):PairedRawTokenContextCaller.explicit(type,keys,values,value);
        if(route==2){try{return (Map<?,?>)(kind==0?Client.class.getMethod("defaults",Class.class,Class.class,Object.class).invoke(null,keys,values,value):Client.class.getMethod("explicit",Class.class,Class.class,Class.class,Object.class).invoke(null,type,keys,values,value));}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(kind==0)return (Map)MethodHandles.publicLookup().findStatic(Client.class,"defaults",MethodType.methodType(Map.class,Class.class,Class.class,Object.class)).invokeExact((Class)keys,(Class)values,value);
        return (Map)MethodHandles.publicLookup().findStatic(Client.class,"explicit",MethodType.methodType(Map.class,Class.class,Class.class,Class.class,Object.class)).invokeExact((Class)type,(Class)keys,(Class)values,value);
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),kind=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final FutureMap input=new FutureMap();final Object value=mode==3||mode==11?input:new Object(),payload=new Object();
        final Class<String> keys=mode==1?null:String.class;final Class<Integer> values=mode==2?null:Integer.class;
        final Class<? extends Map> type=kind==0?(value instanceof Map?input.getClass():HashMap.class):kind==1?TreeMap.class:null;
        final Map<?,?> result=mode==9?null:mode==11?input:new FutureMap();final Object desired=mode==8?new Object():Integer.valueOf(Integer.MIN_VALUE+37+mode);
        final RuntimeException actionFailure=new RuntimeException("owned action failure"),getFailure=new RuntimeException("owned get failure");final Error actionError=new Error("owned action error");
        Trace.failure=new RuntimeException("owned stage failure");Trace.failAt=mode==6?1:mode==7?2:0;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object();Trace.events=0;Trace.kind=Trace.keys=Trace.values=null;Trace.value=Trace.payload=Trace.read=sentinel;Trace.result=input;int calls=Trace.calls;
            Trace.action=new Runnable(){public void run(){require(Trace.events==1&&Trace.kind==type&&Trace.keys==keys&&Trace.values==values&&Trace.value==value&&Trace.payload==sentinel);Trace.hit(3);Trace.payload=payload;Trace.result=result;if(mode==4)throw actionFailure;if(mode==5)throw actionError;}};
            Trace.getAction=new Runnable(){public void run(){require(Trace.events==1324);Trace.read=desired;if(mode==10)throw getFailure;}};
            Map<?,?> actual=input;int answer=42;Throwable failure=null;try{actual=call(route,kind,type,keys,values,value);answer=((Integer)actual.get("owned query")).intValue();}catch(Throwable error){failure=error;}
            long events=mode==6?1:mode==4||mode==5?13:mode==7||mode==9?132:1324;
            require(Trace.events==events&&Trace.calls==calls+1&&Trace.kind==type&&Trace.keys==keys&&Trace.values==values&&Trace.value==value);
            Throwable expected=mode==4?actionFailure:mode==5?actionError:mode==6||mode==7?Trace.failure:mode==10?getFailure:null;
            if(expected!=null)require(failure==expected&&actual==(mode==4||mode==5||mode==6||mode==7?input:result)&&answer==42);
            else if(mode==8)require(failure instanceof ClassCastException&&actual==result&&answer==42);
            else if(mode==9)require(failure instanceof NullPointerException&&actual==null&&answer==42);
            else require(failure==null&&actual==result&&answer==((Integer)desired).intValue());
            require(Trace.payload==(mode==6?sentinel:payload));
            System.out.println("route="+route+",operation="+kind+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
