import fixture.*;
import java.util.Map;
import java.util.AbstractMap;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class PairedSeedScalarHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned paired seed scalar");}
    @SuppressWarnings("rawtypes")
    private static Map.Entry call(int route,int operation,Object key,Object value,boolean flag)throws Throwable {
        if(route==0)return operation==0?Client.call(key,value):Client.full(key,value,flag);
        if(route==1)return operation==0?PairedSeedScalarCaller.call(key,value):PairedSeedScalarCaller.full(key,value,flag);
        if(route==2){try{return (Map.Entry)(operation==0?Client.class.getMethod("call",Object.class,Object.class).invoke(null,key,value):Client.class.getMethod("full",Object.class,Object.class,boolean.class).invoke(null,key,value,flag));}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (Map.Entry)MethodHandles.publicLookup().findStatic(Client.class,"call",MethodType.methodType(Map.Entry.class,Object.class,Object.class)).invokeExact(key,value);
        return (Map.Entry)MethodHandles.publicLookup().findStatic(Client.class,"full",MethodType.methodType(Map.Entry.class,Object.class,Object.class,boolean.class)).invokeExact(key,value,flag);
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Failure runtime=new Failure("owned failure");final Error error=new Error("owned error");Trace.failure=new Failure("owned stage");Trace.failAt=mode==6?1:mode==7?2:0;
        for(int attempt=0;attempt<2;attempt++) {
            final Object key=mode==1?null:new Object(),value=mode==2?null:new String("original"),sentinel=new Object(),replacementKey=new Object();
            final String payload=new String("payload");final boolean flag=operation==1&&mode%2==0;
            final Object desiredValue=mode==9?Integer.valueOf(23):mode==10?null:new String("owned result");
            final Map.Entry desired=mode==3?null:new AbstractMap.SimpleEntry(replacementKey,desiredValue);
            final Throwable[] observed={null};
            Trace.events=0;Trace.created=null;Trace.key=sentinel;Trace.value=sentinel;Trace.payload=sentinel;Trace.previousValue=sentinel;Trace.result=sentinel;Trace.flag=!flag;
            final int calls=Trace.calls,actions=Trace.actions;
            Trace.action=new Runnable(){public void run(){Trace.actions++;require(Trace.events==12&&Trace.key==key&&Trace.value==value&&Trace.flag==flag&&Trace.created!=null&&Trace.created.getKey()==key&&Trace.created.getValue()==value);require(Trace.created.getClass()==(flag?AbstractMap.SimpleImmutableEntry.class:AbstractMap.SimpleEntry.class));Trace.hit(4);Trace.payload=payload;Trace.result=mode==11?Trace.created:desired;if(!flag||mode==8){try{Trace.previousValue=Trace.created.setValue(payload);}catch(UnsupportedOperationException failure){observed[0]=failure;throw failure;}}if(mode==4)throw runtime;if(mode==5)throw error;}};
            Map.Entry result=null;String actual="initial";Integer length=null;Throwable failure=null;
            try{result=call(route,operation,key,value,flag);actual=(String)result.getValue();length=Integer.valueOf(actual.length());}catch(Throwable caught){failure=caught;}
            boolean created=mode!=6,actionReached=mode!=6&&mode!=7,immutableFailure=mode==8&&flag,returned=actionReached&&mode!=4&&mode!=5&&!immutableFailure;
            require(Trace.calls==calls+1&&Trace.actions==actions+(actionReached?1:0)&&Trace.events==(mode==6?1:mode==7?12:returned?1243:124));
            require(Trace.key==key&&Trace.value==value&&Trace.flag==flag&&(Trace.created!=null)==created&&Trace.payload==(actionReached?payload:sentinel));
            require(Trace.result==(actionReached?(mode==11?Trace.created:desired):sentinel));
            require(Trace.previousValue==(actionReached&&!flag?value:sentinel));
            if(created){require(Trace.created.getKey()==key&&Trace.created.getValue()==(actionReached&&!flag?payload:value));require(Trace.created.getClass()==(flag?AbstractMap.SimpleImmutableEntry.class:AbstractMap.SimpleEntry.class));}
            Throwable expected=mode==4?runtime:mode==5?error:mode==6||mode==7?Trace.failure:immutableFailure?observed[0]:null;
            if(expected!=null)require(failure==expected&&result==null&&actual.equals("initial")&&length==null);
            else if(mode==3)require(failure instanceof NullPointerException&&result==null&&actual.equals("initial")&&length==null);
            else if(mode==9)require(failure instanceof ClassCastException&&result==desired&&actual.equals("initial")&&length==null);
            else if(mode==10)require(failure instanceof NullPointerException&&result==desired&&actual==null&&length==null);
            else require(failure==null&&result==(mode==11?Trace.created:desired)&&actual==(mode==11?payload:desiredValue)&&length.intValue()==actual.length());
            if(desired!=null)require(desired.getKey()==replacementKey&&desired.getValue()==desiredValue);
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
