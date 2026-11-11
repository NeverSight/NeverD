import fixture.Core;
import fixture.Trace;
import java.io.File;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ReifiableArrayDelegationHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned reifiable array delegation");}
    private static final class Input extends File {Input(){super("owned-input");}public Path toPath(){Trace.conversions++;Trace.receiver=this;Trace.hit(1);Trace.action.run();Trace.hit(2);return Trace.converted;}}
    private static Core call(int route,int operation,File file,String text,Class<?>[] data)throws Throwable {
        if(route==0)return operation==0?new Core(file,data):new Core(text,data);
        if(route==1)return operation==0?ReifiableArrayDelegationCaller.file(file,data):ReifiableArrayDelegationCaller.text(text,data);
        if(route==2){try{return Core.class.getConstructor(operation==0?File.class:String.class,Class[].class).newInstance(operation==0?file:text,data);}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,File.class,Class[].class)).invokeExact(file,data);
        return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,String.class,Class[].class)).invokeExact(text,data);
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final File file=mode==1?null:new Input();final String text=mode==1?null:"owned-context";
        final Class<?>[] data=mode==3?null:mode==2?new Class<?>[0]:new Class<?>[]{String.class,Integer.class};
        final Path path=mode==3?null:Paths.get("owned-result");final Object desired=mode==8?Integer.valueOf(9):mode==9?null:"result";
        final RuntimeException conversionFailure=new IllegalArgumentException("owned conversion"),targetFailure=new IllegalArgumentException("owned target");final Error conversionError=new Error("owned conversion error"),targetError=new Error("owned target error");
        Trace.failure=new IllegalArgumentException("owned stage");Trace.failAt=mode==6?1:mode==7?2:mode==10?3:mode==11?4:0;
        for(int attempt=0;attempt<2;attempt++) {
            if(data!=null&&data.length!=0)data[0]=String.class;
            final Object sentinel=new Object(),payload=new Object();Trace.events=0;Trace.receiver=null;Trace.path=null;Trace.data=null;Trace.payload=sentinel;Trace.result="warm";Trace.converted=path;int calls=Trace.calls,conversions=Trace.conversions;
            Trace.action=new Runnable(){public void run(){require(Trace.events==1&&Trace.receiver==file&&Trace.payload==sentinel);if(mode==4)throw conversionFailure;if(mode==5)throw conversionError;}};
            Trace.targetAction=new Runnable(){public void run(){require(Trace.events==(operation==0?123:3)&&Trace.path==(operation==0?path:null)&&Trace.data==data&&Trace.payload==sentinel);Trace.payload=payload;Trace.result=desired;if(data!=null&&data.length!=0)data[0]=Double.class;if(mode==4)throw targetFailure;if(mode==5)throw targetError;}};
            Core result=null;int value=42;Throwable failure=null;try{result=call(route,operation,file,text,data);value=((String)result.value).length();}catch(Throwable caught){failure=caught;}
            boolean receiverFailure=operation==0&&mode==1;boolean conversionFails=operation==0&&(mode==4||mode==5||mode==6||mode==7);boolean targetEntered=!receiverFailure&&!conversionFails;
            require(Trace.conversions==conversions+(operation==0&&!receiverFailure?1:0)&&Trace.calls==calls+(targetEntered?1:0));
            require(Trace.receiver==(operation==0&&!receiverFailure?file:null));
            long events=receiverFailure?0:operation==0?mode==4||mode==5||mode==6?1:mode==7?12:mode==10?123:1234:mode==4||mode==5||mode==10?3:34;require(Trace.events==events);
            Throwable expected=mode==4?(operation==0?conversionFailure:targetFailure):mode==5?(operation==0?conversionError:targetError):(operation==0&&(mode==6||mode==7))||mode==10||mode==11?Trace.failure:null;
            if(receiverFailure)require(failure instanceof NullPointerException&&result==null&&value==42);
            else if(expected!=null)require(failure==expected&&result==null&&value==42);
            else if(mode==8)require(failure instanceof ClassCastException&&result!=null&&result.value==desired&&value==42);
            else if(mode==9)require(failure instanceof NullPointerException&&result!=null&&result.value==null&&value==42);
            else require(failure==null&&result!=null&&result.value==desired&&value==6);
            boolean actionRan=targetEntered&&mode!=10;require(Trace.payload==(actionRan?payload:sentinel));
            require(Trace.path==(targetEntered&&operation==0?path:null)&&Trace.data==(targetEntered?data:null));
            if(data!=null&&data.length!=0)require(data[0]==(actionRan?Double.class:String.class)&&data[1]==Integer.class);
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
