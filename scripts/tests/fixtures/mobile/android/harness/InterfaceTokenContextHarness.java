import fixture.Client;
import fixture.Trace;
import java.io.File;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class InterfaceTokenContextHarness {
    private static void require(boolean value){if(!value)throw new AssertionError("owned interface token context");}
    @SuppressWarnings({"rawtypes","unchecked"})
    private static CharSequence call(int route,File context,Class type,CharSequence[] data)throws Throwable {
        if(route==0)return Client.call(context,type,data);if(route==1)return InterfaceTokenContextCaller.call(context,type,data);
        if(route==2){try{return (CharSequence)Client.class.getMethod("call",File.class,Class.class,CharSequence[].class).invoke(null,context,type,data);}catch(InvocationTargetException failure){throw failure.getCause();}}
        return (CharSequence)MethodHandles.publicLookup().findStatic(Client.class,"call",MethodType.methodType(CharSequence.class,File.class,Class.class,CharSequence[].class)).invokeExact(context,type,data);
    }
    @SuppressWarnings("rawtypes")
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final CharSequence[] data=mode==3?null:mode==2?new CharSequence[0]:operation==0?new CharSequence[]{"head","tail"}:new String[]{"head","tail"};
        final File context=mode==1?null:new File(operation==0?"owned-left":"owned-right");
        final Class type=mode==3?null:mode==8?StringBuilder.class:mode==11?Integer.class:String.class;
        final Object desired=mode==8?new StringBuilder("result"):mode==9?null:mode==11?Integer.valueOf(7):"result";
        final RuntimeException actionFailure=new IllegalArgumentException("owned action failure");final Error actionError=new Error("owned action error");
        final IOException checked=new IOException("owned checked failure");
        Trace.failure=new IllegalArgumentException("owned stage failure");Trace.failAt=mode==6?1:mode==7?2:0;Trace.checked=mode==10?checked:null;
        for(int attempt=0;attempt<2;attempt++) {
            if(data!=null&&data.length!=0)data[0]="head";
            final Object sentinel=new Object(),payload=new Object();Trace.events=0;Trace.type=null;Trace.data=null;Trace.context=null;Trace.payload=sentinel;Trace.result="sentinel";int calls=Trace.calls;
            Trace.action=new Runnable(){public void run(){require(Trace.events==1&&Trace.type==type&&Trace.data==data&&Trace.context==context&&Trace.payload==sentinel);Trace.hit(3);Trace.payload=payload;Trace.result=desired;if(data!=null&&data.length!=0)data[0]=mode==10&&operation==1?new StringBuilder("changed"):"changed";if(mode==4)throw actionFailure;if(mode==5)throw actionError;}};
            CharSequence actual="sentinel";int value=42;Throwable failure=null;try{actual=call(route,context,type,data);value=((String)actual).length();}catch(Throwable error){failure=error;}
            require(Trace.calls==calls+1&&Trace.type==type&&Trace.data==data&&Trace.context==context&&Trace.events==(mode==6?1:mode==4||mode==5||mode==10?13:132));
            Throwable expected=mode==4?actionFailure:mode==5?actionError:mode==6||mode==7?Trace.failure:mode==10&&operation==0?checked:null;
            if(expected!=null)require(failure==expected&&actual.equals("sentinel")&&value==42);
            else if(mode==10)require(failure instanceof ArrayStoreException&&actual.equals("sentinel")&&value==42);
            else if(mode==11)require(failure instanceof ClassCastException&&actual.equals("sentinel")&&value==42);
            else if(mode==8)require(failure instanceof ClassCastException&&actual==desired&&value==42);
            else if(mode==9)require(failure instanceof NullPointerException&&actual==null&&value==42);
            else require(failure==null&&actual==desired&&value==6);
            require(Trace.payload==(mode==6?sentinel:payload));
            if(data!=null&&data.length!=0)require(data[0].equals(mode==6||mode==10&&operation==1?"head":"changed")&&data[1].equals("tail"));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
