import fixture.*;
import java.util.Map;
import java.util.HashMap;
import java.util.AbstractMap;
import java.util.Collections;
import java.util.Set;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class CarrierNominalVarargsHarness {
    private static void require(boolean value){if(!value)throw new AssertionError("owned carrier nominal varargs");}
    @SuppressWarnings({"rawtypes","unchecked"})
    private static String call(int route,int operation,Map values,String first,String second,String[] tail)throws Throwable {
        if(route==0)return operation==0?Client.call(values,first,second):Client.full(values,first,second,tail);
        if(route==1)return operation==0?CarrierNominalVarargsCaller.call(values,first,second):CarrierNominalVarargsCaller.full(values,first,second,tail);
        if(route==2){try{return operation==0?(String)Client.class.getMethod("call",Map.class,String.class,String.class).invoke(null,values,first,second):(String)Client.class.getMethod("full",Map.class,String.class,String.class,String[].class).invoke(null,values,first,second,tail);}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (String)MethodHandles.publicLookup().findStatic(Client.class,"call",MethodType.methodType(String.class,Map.class,String.class,String.class)).invokeExact(values,first,second);
        return (String)MethodHandles.publicLookup().findStatic(Client.class,"full",MethodType.methodType(String.class,Map.class,String.class,String.class,String[].class)).invokeExact(values,first,second,tail);
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Map values=mode==1?null:mode==10?new AbstractMap(){public Set entrySet(){throw new AssertionError("owned carrier nominal varargs");}}:mode==11?Collections.unmodifiableMap(new HashMap<Object,Object>()):new HashMap<Object,Object>();
        final String first=mode==2?null:new String(" first-"+mode+" "),second=mode==3?null:new String(" second-"+mode+" ");
        final String originalA=new String("a"),originalB=new String("b"),replacement=new String("changed");
        final String[] tail=mode==2?null:mode==3?new String[0]:new String[]{originalA,originalB};
        final Failure actionFailure=new Failure("owned action failure");final Error actionError=new Error("owned action error");
        final String desired=mode==9?null:mode==8?new String(""):new String("owned result"),initial=new String("initial");
        Trace.failure=new Failure("owned stage failure");Trace.failAt=mode==6?1:mode==7?2:0;
        String[] previous=null;
        for(int attempt=0;attempt<2;attempt++){
            if(values!=null&&mode!=10&&mode!=11)values.clear();if(tail!=null&&tail.length>0){tail[0]=originalA;tail[1]=originalB;}
            final Object sentinel=new Object(),payload=new Object();Trace.events=0;Trace.values=null;Trace.first=initial;Trace.second=initial;Trace.tail=null;Trace.payload=sentinel;Trace.result=initial;int calls=Trace.calls;
            Trace.action=new Runnable(){public void run(){require(Trace.events==1&&Trace.values==values&&Trace.first==first&&Trace.second==second&&Trace.payload==sentinel);require(operation==0?Trace.tail!=null&&Trace.tail.length==0:Trace.tail==tail);Trace.hit(3);Trace.payload=payload;Trace.result=desired;if(values!=null&&mode!=10)values.put(payload,payload);if(Trace.tail!=null&&Trace.tail.length>0)Trace.tail[0]=replacement;if(mode==4)throw actionFailure;if(mode==5)throw actionError;}};
            String actual=initial;Character ch=null;Throwable failure=null;try{actual=call(route,operation,values,first,second,tail);ch=Character.valueOf(actual.charAt(0));}catch(Throwable error){failure=error;}
            require(Trace.calls==calls+1&&Trace.values==values&&Trace.first==first&&Trace.second==second&&Trace.events==(mode==6?1:mode==4||mode==5||mode==11?13:132));
            if(operation==0){require(Trace.tail!=null&&Trace.tail.length==0&&Trace.tail!=previous);previous=Trace.tail;}else require(Trace.tail==tail);
            Throwable expected=mode==4?actionFailure:mode==5?actionError:mode==6||mode==7?Trace.failure:null;
            if(expected!=null)require(failure==expected&&actual==initial&&ch==null);
            else if(mode==11)require(failure instanceof UnsupportedOperationException&&actual==initial&&ch==null);
            else if(mode==8)require(failure instanceof StringIndexOutOfBoundsException&&actual==desired&&ch==null);
            else if(mode==9)require(failure instanceof NullPointerException&&actual==null&&ch==null);
            else require(failure==null&&actual==desired&&ch.charValue()=='o');
            require(Trace.payload==(mode==6?sentinel:payload)&&Trace.result==(mode==6?initial:desired));
            if(values!=null&&mode!=10)require(values.size()==(mode==6||mode==11?0:1));
            if(tail!=null&&tail.length>0)require(tail[0]==(operation==1&&mode!=6&&mode!=11?replacement:originalA)&&tail[1]==originalB);
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
