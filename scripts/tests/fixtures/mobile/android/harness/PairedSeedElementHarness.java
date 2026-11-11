import fixture.*;
import java.util.Map;
import java.util.HashMap;
import java.util.function.Function;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class PairedSeedElementHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned paired seed element");}
    @SuppressWarnings({"rawtypes","unchecked"})
    private static String call(int route,int operation,Map values,Object key,Function function)throws Throwable {
        if(route==0)return (String)(operation==0?Client.call(values,key,function):Client.full(values,key,function));
        if(route==1)return operation==0?PairedSeedElementCaller.call(values,key,function):PairedSeedElementCaller.full(values,key,function);
        if(route==2){try{return (String)Client.class.getMethod(operation==0?"call":"full",Map.class,Object.class,Function.class).invoke(null,values,key,function);}catch(InvocationTargetException failure){throw failure.getCause();}}
        return (String)(Object)MethodHandles.publicLookup().findStatic(Client.class,operation==0?"call":"full",MethodType.methodType(Object.class,Map.class,Object.class,Function.class)).invokeExact(values,key,function);
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Failure runtime=new Failure("owned failure");final Error error=new Error("owned error");Trace.failure=new Failure("owned stage");Trace.failAt=mode==6?1:mode==7?3:0;
        for(int attempt=0;attempt<2;attempt++) {
            final Map values=mode==1?null:new HashMap<Object,Object>();final Object key=mode==2?null:new Object(),sentinel=new Object(),payload=new Object(),callbackPayload=new Object(),partial=new Object();
            final Object computed=mode==9?Integer.valueOf(17):new String("computed"),desired=mode==10?Integer.valueOf(23):mode==11?null:new String("owned result"),early=new String("early");
            Trace.events=0;Trace.values=null;Trace.key=sentinel;Trace.function=null;Trace.payload=sentinel;Trace.callbackPayload=sentinel;Trace.callbackResult=sentinel;Trace.result=early;
            final int calls=Trace.calls,callbacks=Trace.callbackCalls;
            final Function function=mode==3?null:new Function(){public Object apply(Object arg){Trace.callbackCalls++;require(arg==key&&Trace.events==12&&Trace.function==this&&Trace.dynamicFunction==payload);Trace.hit(5);Trace.callbackPayload=callbackPayload;Trace.result=desired;Trace.dynamicFunction=callbackPayload;if(values!=null)values.put("partial",partial);if(mode==8)throw runtime;return computed;}};
            Trace.dynamicFunction=function;
            Trace.action=new Runnable(){public void run(){require(Trace.events==1&&Trace.values==values&&Trace.key==key&&Trace.function==function&&Trace.dynamicFunction==function);Trace.payload=payload;Trace.dynamicFunction=payload;if(mode==4)throw runtime;if(mode==5)throw error;}};
            String actual="initial";Character ch=null;Throwable failure=null;try{actual=call(route,operation,values,key,function);ch=Character.valueOf(actual.charAt(0));}catch(Throwable caught){failure=caught;}
            boolean actionReached=mode!=6,callbackReached=mode!=3&&mode!=4&&mode!=5&&mode!=6,callbackReturned=callbackReached&&mode!=8,stored=callbackReturned&&mode!=1&&mode!=7;
            long events=mode==4||mode==5||mode==6?1:mode==3?12:mode==8?125:mode==1||mode==7?1253:12534;
            require(Trace.calls==calls+1&&Trace.callbackCalls==callbacks+(callbackReached?1:0)&&Trace.events==events&&Trace.values==values&&Trace.key==key&&Trace.function==function);
            Throwable expected=mode==4||mode==8?runtime:mode==5?error:mode==6||mode==7?Trace.failure:null;
            if(expected!=null)require(failure==expected&&actual.equals("initial")&&ch==null);
            else if(mode==1||mode==3)require(failure instanceof NullPointerException&&actual.equals("initial")&&ch==null);
            else if(mode==10)require(failure instanceof ClassCastException&&actual.equals("initial")&&ch==null);
            else if(mode==11)require(failure instanceof NullPointerException&&actual==null&&ch==null);
            else require(failure==null&&actual==desired&&ch.charValue()=='o');
            require(Trace.payload==(actionReached?payload:sentinel)&&Trace.callbackPayload==(callbackReached?callbackPayload:sentinel)&&Trace.callbackResult==(callbackReturned?computed:sentinel)&&Trace.result==(callbackReached?desired:early));
            if(values!=null){require(values.size()==(callbackReached?1:0)+(stored?1:0));if(callbackReached)require(values.get("partial")==partial);if(stored)require(values.get(key)==computed);}
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
