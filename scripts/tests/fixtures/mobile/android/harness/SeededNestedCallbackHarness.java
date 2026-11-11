import fixture.*;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.util.ArrayList;
import java.util.List;
public final class SeededNestedCallbackHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned seeded nested callback");}
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure)throws X{throw (X)failure;}
    private static final class Input {}
    private static final class Value {}
    private static final class Result extends ArrayList<Object> {
        public Object get(int index){Trace.reads++;require(Trace.events==123);return super.get(index);}
    }
    private static List call(int route,int operation,Object values,Object seed,Context context,Sink sink)throws Throwable {
        if(route==0)return operation==0?Core.call((List)values,seed,context,sink):Core.checked(values,seed,context,sink);
        if(route==1)return operation==0?SeededNestedCallbackCaller.call((List)values,seed,context,sink):SeededNestedCallbackCaller.checked(values,seed,context,sink);
        if(route==2){try{return (List)Core.class.getMethod(operation==0?"call":"checked",operation==0?List.class:Object.class,Object.class,Context.class,Sink.class).invoke(null,values,seed,context,sink);}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (List)MethodHandles.publicLookup().findStatic(Core.class,"call",MethodType.methodType(List.class,List.class,Object.class,Context.class,Sink.class)).invokeExact((List)values,seed,context,sink);
        return (List)MethodHandles.publicLookup().findStatic(Core.class,"checked",MethodType.methodType(List.class,Object.class,Object.class,Context.class,Sink.class)).invokeExact(values,seed,context,sink);
    }
    @SuppressWarnings("unchecked") public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final RuntimeException runtime=new IllegalArgumentException("owned runtime");final Error error=new Error("owned error");final IOException checked=new IOException("owned checked");Trace.failure=new IllegalArgumentException("owned stage");
        for(int attempt=0;attempt<2;attempt++) {
            final Value seed=mode==2?null:new Value(),changed=new Value();final Object sentinel=new Object(),payload=new Object(),raw=new Object();
            final List values=mode==1?null:new ArrayList();if(values!=null)values.add(raw);
            final Object lateInput=values==null?null:mode==6?raw:new Input();
            final Context context=mode==2?null:new Context(payload),oldContext=new Context(sentinel);
            final Target target=new Target(seed),resultElement=new Target(mode==11?raw:seed);
            final List desired=mode==3?null:new Result();if(desired!=null)desired.add(resultElement);
            final Object input=mode==7&&operation==1?new Object():values;
            Trace.events=0;Trace.values=null;Trace.seed=sentinel;Trace.context=oldContext;Trace.sink=null;Trace.target=null;Trace.result=null;Trace.payload=sentinel;Trace.failAt=mode==9?1:mode==10?2:0;
            final int actions=Trace.actions,callbacks=Trace.callbacks,reads=Trace.reads;
            final Sink<Input,Value> sink=new Sink<Input,Value>(){public void accept(Input value,Target<Value> output){Trace.callbacks++;require(Trace.events==12&&value==lateInput&&output==target&&output.value==seed&&Trace.seed==seed&&Trace.context==context&&Trace.payload==payload);output.value=changed;Trace.result=desired;if(mode==8)SeededNestedCallbackHarness.<RuntimeException>fail(checked);}};
            Trace.action=new Runnable(){public void run(){Trace.actions++;require(Trace.events==1&&Trace.values==values&&Trace.seed==seed&&Trace.context==context&&Trace.sink==sink&&Trace.payload==sentinel);Trace.payload=payload;Trace.target=target;Trace.result=desired;if(values!=null)values.set(0,lateInput);if(mode==4)throw runtime;if(mode==5)throw error;}};
            List result=null;Value value=null;Throwable failure=null;try{result=call(route,operation,input,seed,context,sink);value=(Value)((Target)result.get(0)).value;}catch(Throwable caught){failure=caught;}
            final boolean bad=mode==7&&operation==1,captured=!bad&&mode!=9,action=captured,callback=action&&mode!=4&&mode!=5&&mode!=10&&mode!=6,returned=callback&&mode!=8;
            require(Trace.events==(bad?0:mode==9||mode==4||mode==5?1:mode==6||mode==8||mode==10?12:123));
            require(Trace.actions==actions+(action?1:0)&&Trace.callbacks==callbacks+(callback?1:0)&&Trace.reads==reads+(returned&&desired!=null?1:0));
            Throwable wanted=mode==4?runtime:mode==5?error:mode==8?checked:mode==9||mode==10?Trace.failure:null;
            if(wanted!=null)require(failure==wanted&&result==null);else if(bad||mode==6)require(failure instanceof ClassCastException&&result==null);else if(mode==3)require(failure instanceof NullPointerException&&result==null);else if(mode==11)require(failure instanceof ClassCastException&&result==desired&&value==null);else require(failure==null&&result==desired&&value==seed);
            require(Trace.values==(captured?values:null)&&Trace.seed==(captured?seed:sentinel)&&Trace.context==(captured?context:oldContext)&&Trace.sink==(captured?sink:null)&&Trace.target==(action?target:null)&&Trace.payload==(action?payload:sentinel)&&Trace.result==(action?desired:null));
            require(target.value==(callback?changed:seed)&&resultElement.value==(mode==11?raw:seed));if(values!=null)require(values.get(0)==(action?lateInput:raw));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
