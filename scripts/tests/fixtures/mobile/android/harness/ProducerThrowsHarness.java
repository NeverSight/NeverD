import fixture.Client;
import fixture.Provider;
import fixture.Handler;
import fixture.Trace;
import fixture.Failure;
import java.io.BufferedReader;
import java.io.StringReader;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ProducerThrowsHarness {
    private static void require(boolean value){if(!value)throw new AssertionError("owned producer throws");}
    private static final class Derived extends Provider {
        @Override public <T> T read(Handler<T> handler){Trace.dispatch++;return super.read(handler);}
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    private static Object call(int route,int operation,Provider receiver,Handler handler)throws Throwable {
        if(route==0)return operation==0?Client.call(receiver,handler):Client.ignore(receiver,handler);
        if(route==1)return operation==0?ProducerThrowsCaller.call(receiver,handler):ProducerThrowsCaller.ignore(receiver,handler);
        String name=operation==0?"call":"ignore";
        if(route==2){try{return Client.class.getMethod(name,Provider.class,Handler.class).invoke(null,receiver,handler);}catch(InvocationTargetException failure){throw failure.getCause();}}
        return (Object)MethodHandles.publicLookup().findStatic(Client.class,name,MethodType.methodType(Object.class,Provider.class,Handler.class)).invokeExact(receiver,handler);
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Provider receiver=mode==1?null:mode==10?new Derived():new Provider();
        final BufferedReader reader=mode==3?null:new BufferedReader(new StringReader("owned"));
        final Object desired=mode==8?Integer.valueOf(47):mode==9?null:new String("owned-"+mode);
        final Failure actionFailure=new Failure("owned callback failure"),wrapper=new Failure("owned checked wrapper");final Error actionError=new Error("owned callback error");
        final IOException checked=new IOException("owned checked failure");wrapper.initCause(checked);
        final Handler handler=mode==2?null:new Handler(){public Object read(BufferedReader actual)throws IOException {require(Trace.events==13&&actual==reader);Trace.callbackCalls++;Trace.reader=actual;if(mode==4)throw actionFailure;if(mode==5)throw actionError;if(mode==11)throw checked;return desired;}};
        Trace.failure=new Failure("owned stage failure");Trace.failAt=mode==6?1:mode==7?2:0;
        for(int attempt=0;attempt<2;attempt++){
            final Object sentinel=new Object(),payload=new Object();Trace.events=0;Trace.receiver=sentinel;Trace.handler=null;Trace.result=sentinel;Trace.marker=sentinel;Trace.payload=sentinel;Trace.reader=null;int calls=Trace.calls,dispatch=Trace.dispatch,callbacks=Trace.callbackCalls;
            Trace.action=new Runnable(){public void run(){require(Trace.events==1&&Trace.receiver==receiver&&Trace.handler==handler&&Trace.payload==sentinel);Trace.hit(3);Trace.payload=payload;Trace.marker=payload;try{Trace.result=handler.read(reader);}catch(RuntimeException|Error error){throw error;}catch(IOException error){require(error==checked);throw wrapper;}}};
            Object actual=sentinel;int length=-7;Throwable failure=null;try{actual=call(route,operation,receiver,handler);if(operation==0)length=((String)actual).length();}catch(Throwable error){failure=error;}
            if(mode==1){require(failure instanceof NullPointerException&&actual==sentinel&&length==-7&&Trace.calls==calls&&Trace.dispatch==dispatch&&Trace.callbackCalls==callbacks&&Trace.events==0&&Trace.receiver==sentinel&&Trace.handler==null&&Trace.payload==sentinel&&Trace.marker==sentinel&&Trace.result==sentinel);}
            else {
                require(Trace.calls==calls+1&&Trace.dispatch==dispatch+(mode==10?1:0)&&Trace.receiver==receiver&&Trace.handler==handler&&Trace.events==(mode==6?1:mode==2||mode==4||mode==5||mode==11?13:132));
                Throwable expected=mode==4?actionFailure:mode==5?actionError:mode==6||mode==7?Trace.failure:mode==11?wrapper:null;
                if(expected!=null)require(failure==expected&&actual==sentinel&&length==-7);
                else if(mode==2)require(failure instanceof NullPointerException&&actual==sentinel&&length==-7);
                else if(operation==0&&mode==8)require(failure instanceof ClassCastException&&actual==desired&&length==-7);
                else if(operation==0&&mode==9)require(failure instanceof NullPointerException&&actual==null&&length==-7);
                else require(failure==null&&actual==(operation==0?desired:payload)&&(operation==1||length==((String)desired).length()));
                boolean callback=mode!=2&&mode!=6;require(Trace.callbackCalls==callbacks+(callback?1:0));
                require(Trace.payload==(mode==6?sentinel:payload)&&Trace.marker==(mode==6?sentinel:payload));
                require(Trace.result==(mode==2||mode==4||mode==5||mode==6||mode==11?sentinel:desired));
                require(Trace.reader==(callback?reader:null));if(mode==11)require(failure.getCause()==checked);
            }
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
