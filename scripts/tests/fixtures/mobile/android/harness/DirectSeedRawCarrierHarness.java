import fixture.*;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class DirectSeedRawCarrierHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned direct seed raw carrier");}
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure)throws X{throw (X)failure;}
    private static final class Value {}
    private static Node call(int route,int operation,Object value,Object previous)throws Throwable {
        if(route==0)return operation==0?Core.call(value,(Context)previous):Core.checked(value,previous);
        if(route==1)return operation==0?DirectSeedRawCarrierCaller.call(value,(Context)previous):DirectSeedRawCarrierCaller.checked(value,previous);
        if(route==2){try{return (Node)Core.class.getMethod(operation==0?"call":"checked",Object.class,operation==0?Context.class:Object.class).invoke(null,value,previous);}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (Node)MethodHandles.publicLookup().findStatic(Core.class,"call",MethodType.methodType(Node.class,Object.class,Context.class)).invokeExact(value,(Context)previous);
        return (Node)MethodHandles.publicLookup().findStatic(Core.class,"checked",MethodType.methodType(Node.class,Object.class,Object.class)).invokeExact(value,previous);
    }
    @SuppressWarnings("unchecked") public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final RuntimeException runtime=new IllegalArgumentException("owned runtime");final Error error=new Error("owned error");final IOException checked=new IOException("owned checked");Trace.failure=new IllegalArgumentException("owned stage");
        for(int attempt=0;attempt<2;attempt++) {
            Trace.failAt=0;Trace.action=new Runnable(){public void run(){}};
            final Value value=mode==2?null:new Value();final Object sentinel=new Object(),payload=new Object();
            final Context previous=mode==1?null:new Context("owned-unrelated");
            final Object desiredValue=mode==11?new Object():value;final Node desired=mode==3?null:new Node(desiredValue,previous);
            final Object input=mode==7&&operation==1?new Object():previous;
            Trace.events=0;Trace.constructed=null;Trace.created=null;Trace.result=null;Trace.payload=sentinel;Trace.failAt=mode==9?1:mode==10?2:mode==6?3:0;
            final int constructors=Trace.constructors,actions=Trace.actions;
            Trace.action=new Runnable(){public void run(){Trace.actions++;require(Trace.events==1&&Trace.constructed!=null&&Trace.constructed.value==value&&Trace.constructed.previous==previous&&Trace.created==null&&Trace.payload==sentinel);Trace.payload=payload;Trace.result=desired;Trace.constructed.value=payload;if(mode==4)throw runtime;if(mode==5)throw error;if(mode==8)DirectSeedRawCarrierHarness.<RuntimeException>fail(checked);}};
            Node result=null;Value output=null;Throwable failure=null;try{result=call(route,operation,value,input);output=(Value)result.value;}catch(Throwable caught){failure=caught;}
            final boolean bad=mode==7&&operation==1,entered=!bad,action=entered&&mode!=9,finished=action&&mode!=4&&mode!=5&&mode!=8&&mode!=10,returned=finished&&mode!=6;
            require(Trace.events==(bad?0:mode==9||mode==4||mode==5||mode==8?1:mode==10?12:123));
            require(Trace.constructors==constructors+(entered?1:0)&&Trace.actions==actions+(action?1:0));
            Throwable wanted=mode==4?runtime:mode==5?error:mode==8?checked:mode==9||mode==10||mode==6?Trace.failure:null;
            if(wanted!=null)require(failure==wanted&&result==null);else if(bad)require(failure instanceof ClassCastException);else if(mode==3)require(failure instanceof NullPointerException&&result==null);else if(mode==11)require(failure instanceof ClassCastException&&result==desired&&output==null);else require(failure==null&&result==desired&&output==value);
            require((Trace.constructed!=null)==action&&(Trace.created!=null)==finished&&Trace.payload==(action?payload:sentinel)&&Trace.result==(action?desired:null));
            if(action)require(Trace.constructed.value==payload&&Trace.constructed.previous==previous&&(Object)Trace.constructed!=previous&&Trace.constructed!=desired);
            if(finished)require(Trace.created==Trace.constructed);if(previous!=null)require(previous.value.equals("owned-unrelated"));if(returned&&desired!=null)require(desired.value==desiredValue);
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
