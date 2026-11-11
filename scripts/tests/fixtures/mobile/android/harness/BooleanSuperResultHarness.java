import fixture.*;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class BooleanSuperResultHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned boolean super result");}
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure)throws X{throw (X)failure;}
    private static final class FutureCore extends Core {public boolean change(String key,Object expected,Object update){throw new AssertionError("owned boolean super result virtual redispatch");}}
    private static boolean call(int route,int operation,Core receiver,Object value,Object expected,Object update)throws Throwable {
        if(route==0)return operation==0?receiver.submit((String)value,expected,update):receiver.checked(value,expected,update);
        if(route==1)return operation==0?BooleanSuperResultCaller.submit(receiver,(String)value,expected,update):BooleanSuperResultCaller.checked(receiver,value,expected,update);
        if(route==2){try{return (Boolean)Core.class.getMethod(operation==0?"submit":"checked",operation==0?String.class:Object.class,Object.class,Object.class).invoke(receiver,value,expected,update);}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (boolean)MethodHandles.publicLookup().findVirtual(Core.class,"submit",MethodType.methodType(boolean.class,String.class,Object.class,Object.class)).invokeExact(receiver,(String)value,expected,update);
        return (boolean)MethodHandles.publicLookup().findVirtual(Core.class,"checked",MethodType.methodType(boolean.class,Object.class,Object.class,Object.class)).invokeExact(receiver,value,expected,update);
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Core receiver=mode==1?null:new FutureCore();final Object input=mode==2?null:mode==6&&operation==1?new Object():new String("owned-input");
        final IOException checked=new IOException("owned checked");final RuntimeException runtime=new IllegalArgumentException("owned runtime");final Error error=new Error("owned error");
        Trace.failure=new IllegalArgumentException("owned stage");Trace.failAt=mode>=8?mode-7:0;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),expected=mode==2?null:new Object(),update=mode==3?expected:new Object(),payload=new Object(),normalizationPayload=new Object();
            final String desiredKey=mode==2||mode==3?null:new String("owned-normalized"),oldKey=new String("owned-old-key");final boolean desired=mode!=3&&attempt==0;
            Trace.events=0;Trace.receiver=null;Trace.raw=sentinel;Trace.key=sentinel;Trace.expected=sentinel;Trace.update=sentinel;Trace.payload=sentinel;Trace.normalizationPayload=sentinel;Trace.normalized=oldKey;Trace.result=!desired;
            final int normalizations=Trace.normalizations,actions=Trace.actions;
            Trace.normalizeAction=new Runnable(){public void run(){Trace.normalizations++;require(Trace.events==1&&Trace.raw==input&&Trace.receiver==null&&Trace.payload==sentinel);Trace.normalized=desiredKey;Trace.normalizationPayload=normalizationPayload;if(mode==7)BooleanSuperResultHarness.<RuntimeException>fail(checked);}};
            Trace.action=new Runnable(){public void run(){Trace.actions++;require(Trace.events==12&&Trace.raw==input&&Trace.receiver==receiver&&Trace.key==desiredKey&&Trace.expected==expected&&Trace.update==update&&Trace.payload==sentinel);Trace.payload=payload;Trace.result=desired;if(mode==4)throw runtime;if(mode==5)throw error;}};
            boolean result=!desired;Throwable failure=null;try{result=call(route,operation,receiver,input,expected,update);}catch(Throwable caught){failure=caught;}
            final boolean bad=mode==6&&operation==1,normalized=mode!=1&&!bad&&mode!=8,parent=normalized&&mode!=7&&mode!=9;
            final long events=mode==1||bad?0:mode==7||mode==8?1:mode==4||mode==5||mode==9?12:mode==10?123:1234;
            require(Trace.events==events&&Trace.normalizations==normalizations+(normalized?1:0)&&Trace.actions==actions+(parent?1:0));
            Throwable wanted=mode==4?runtime:mode==5?error:mode==7?checked:mode>=8?Trace.failure:null;
            if(mode==1)require(failure instanceof NullPointerException);else if(bad)require(failure instanceof ClassCastException);else require(failure==wanted);
            require(Trace.raw==(normalized?input:sentinel)&&Trace.normalizationPayload==(normalized?normalizationPayload:sentinel)&&Trace.normalized==(normalized?desiredKey:oldKey));
            require(Trace.receiver==(parent?receiver:null)&&Trace.key==(parent?desiredKey:sentinel)&&Trace.expected==(parent?expected:sentinel)&&Trace.update==(parent?update:sentinel)&&Trace.payload==(parent?payload:sentinel)&&Trace.result==(parent?desired:!desired));
            require(result==(failure==null?desired:!desired));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
