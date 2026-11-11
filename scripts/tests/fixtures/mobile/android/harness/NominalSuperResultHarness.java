import fixture.*;
import java.io.IOException;
import java.nio.file.FileVisitResult;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class NominalSuperResultHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned nominal super result");}
    private static final class FutureCore extends Core {public FileVisitResult visit(String value,IOException error){throw new AssertionError("virtual redispatch");}}
    private static FileVisitResult call(int route,int operation,Core receiver,Object value,IOException error)throws Throwable {
        if(route==0)return operation==0?receiver.submit((String)value,error):receiver.checked(value,error);
        if(route==1)return operation==0?NominalSuperResultCaller.submit(receiver,(String)value,error):NominalSuperResultCaller.checked(receiver,value,error);
        if(route==2){try{return (FileVisitResult)Core.class.getMethod(operation==0?"submit":"checked",operation==0?String.class:Object.class,IOException.class).invoke(receiver,value,error);}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (FileVisitResult)MethodHandles.publicLookup().findVirtual(Core.class,"submit",MethodType.methodType(FileVisitResult.class,String.class,IOException.class)).invokeExact(receiver,(String)value,error);
        return (FileVisitResult)MethodHandles.publicLookup().findVirtual(Core.class,"checked",MethodType.methodType(FileVisitResult.class,Object.class,IOException.class)).invokeExact(receiver,value,error);
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Core receiver=mode==1?null:new FutureCore();final Object input=mode==2?null:mode==6&&operation==1?new Object():new String("owned-input");
        final IOException checked=new IOException("owned checked");final RuntimeException runtime=new IllegalArgumentException("owned callback");final Error error=new Error("owned callback error");
        Trace.failure=new IllegalArgumentException("owned stage");Trace.failAt=mode>=8?mode-7:0;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object();final FileVisitResult desired=mode==3?null:attempt==0?FileVisitResult.SKIP_SUBTREE:FileVisitResult.TERMINATE;
            Trace.events=0;Trace.value=sentinel;Trace.error=null;Trace.result=FileVisitResult.CONTINUE;
            Trace.action=new Runnable(){public void run(){require(Trace.events==12&&Trace.value==input&&Trace.error==(mode==7?checked:null));Trace.result=desired;if(mode==4)throw runtime;if(mode==5)throw error;}};
            FileVisitResult result=FileVisitResult.CONTINUE;Throwable failure=null;
            try{result=call(route,operation,receiver,input,mode==7?checked:null);}catch(Throwable caught){failure=caught;}
            boolean bad=mode==6&&operation==1;
            long events=mode==1?0:bad||mode==8?1:mode==4||mode==5||mode==7||mode==9?12:mode==10?123:1234;
            require(Trace.events==events);
            Throwable expected=mode==4?runtime:mode==5?error:mode==7?checked:mode>=8?Trace.failure:null;
            if(mode==1)require(failure instanceof NullPointerException);
            else if(bad)require(failure instanceof ClassCastException);
            else require(failure==expected);
            boolean entered=mode!=1&&!bad&&mode!=8&&mode!=9;
            require(Trace.value==(entered?input:sentinel)&&Trace.error==(entered&&mode==7?checked:null));
            boolean actionRan=entered;
            require(Trace.result==(actionRan?desired:FileVisitResult.CONTINUE));
            if(failure==null)require(result==desired);else require(result==FileVisitResult.CONTINUE);
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
