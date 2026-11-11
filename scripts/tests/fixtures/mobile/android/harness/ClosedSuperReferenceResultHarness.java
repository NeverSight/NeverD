import fixture.*;
import java.io.IOException;
import java.util.ArrayList;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ClosedSuperReferenceResultHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned nominal super result");}
    private static final class FutureCore extends Core {
        public ArrayList<String> visit(String value,ArrayList<String> items,IOException error){throw new AssertionError("virtual redispatch");}
    }
    private static Object call(int route,int operation,Core receiver,Object value,Object items,IOException error)throws Throwable {
        if(route==0){
            if(operation==0)return receiver.submit((String)value,(ArrayList<String>)items,error);
            if(operation==1)return receiver.checked(value,items,error);
            receiver.discard((String)value,(ArrayList<String>)items,error);return null;
        }
        if(route==1){
            if(operation==0)return ClosedSuperReferenceResultCaller.submit(receiver,(String)value,(ArrayList<String>)items,error);
            if(operation==1)return ClosedSuperReferenceResultCaller.checked(receiver,value,items,error);
            ClosedSuperReferenceResultCaller.discard(receiver,(String)value,(ArrayList<String>)items,error);return null;
        }
        String name=operation==0?"submit":operation==1?"checked":"discard";
        if(route==2){try{return Core.class.getMethod(name,operation==1?Object.class:String.class,operation==1?Object.class:ArrayList.class,IOException.class).invoke(receiver,value,items,error);}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (ArrayList<String>)MethodHandles.publicLookup().findVirtual(Core.class,name,MethodType.methodType(ArrayList.class,String.class,ArrayList.class,IOException.class)).invokeExact(receiver,(String)value,(ArrayList)items,error);
        if(operation==1)return (ArrayList<String>)MethodHandles.publicLookup().findVirtual(Core.class,name,MethodType.methodType(ArrayList.class,Object.class,Object.class,IOException.class)).invokeExact(receiver,value,items,error);
        MethodHandles.publicLookup().findVirtual(Core.class,name,MethodType.methodType(void.class,String.class,ArrayList.class,IOException.class)).invokeExact(receiver,(String)value,(ArrayList)items,error);return null;
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Core receiver=mode==1?null:new FutureCore();
        final Object input=mode==2?null:mode==12&&operation==1?new Object():new String("owned-input");
        final Object items=mode==3?null:mode==13&&operation==1?new Object():new ArrayList<String>();
        final IOException checked=new IOException("owned checked");final RuntimeException runtime=new IllegalArgumentException("owned callback");final Error error=new Error("owned callback error");
        Trace.failure=new IllegalArgumentException("owned stage");Trace.failAt=mode>=8&&mode<=11?mode-7:0;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object();
            final Object desired=mode==4?null:mode==14?new Object():mode==15?items:new ArrayList<String>();
            Trace.events=0;Trace.value=sentinel;Trace.items=sentinel;Trace.error=null;Trace.result=sentinel;
            Trace.action=new Runnable(){public void run(){require(Trace.events==12&&Trace.value==input&&Trace.items==items&&Trace.error==(mode==7?checked:null));Trace.result=desired;if(mode==5)throw runtime;if(mode==6)throw error;}};
            Object result=sentinel;Throwable failure=null;
            try{result=call(route,operation,receiver,input,items,mode==7?checked:null);}catch(Throwable caught){failure=caught;}
            boolean bad=(mode==12||mode==13)&&operation==1;
            boolean castResult=mode==14&&operation!=2;
            long events=mode==1?0:bad||mode==8?1:mode==5||mode==6||mode==7||mode==9?12:mode==10||castResult?123:1234;
            require(Trace.events==events);
            Throwable expected=mode==5?runtime:mode==6?error:mode==7?checked:mode>=8&&mode<=11?Trace.failure:null;
            if(mode==1)require(failure instanceof NullPointerException);
            else if(bad||castResult)require(failure instanceof ClassCastException);
            else require(failure==expected);
            boolean entered=mode!=1&&!bad&&mode!=8&&mode!=9;
            require(Trace.value==(entered?input:sentinel)&&Trace.items==(entered?items:sentinel)&&Trace.error==(entered&&mode==7?checked:null));
            require(Trace.result==(entered?desired:sentinel));
            if(failure==null)require(result==(operation==2?null:desired));else require(result==sentinel);
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
