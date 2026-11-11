import fixture.*;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.util.ArrayList;
import java.util.List;
public final class NestedSeedTransferHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned nested seed transfer");}
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure)throws X{throw (X)failure;}
    private static final class Value {}
    private static final class Result extends ArrayList<Object> {
        public Object get(int index){Trace.reads++;require(Trace.events==12);return super.get(index);}
    }
    private static List call(int route,int operation,Object values,Object seed)throws Throwable {
        if(route==0)return operation==0?Core.call((List)values,seed):Core.checked(values,seed);
        if(route==1)return operation==0?NestedSeedTransferCaller.call((List)values,seed):NestedSeedTransferCaller.checked(values,seed);
        if(route==2){try{return (List)Core.class.getMethod(operation==0?"call":"checked",operation==0?List.class:Object.class,Object.class).invoke(null,values,seed);}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (List)MethodHandles.publicLookup().findStatic(Core.class,"call",MethodType.methodType(List.class,List.class,Object.class)).invokeExact((List)values,seed);
        return (List)MethodHandles.publicLookup().findStatic(Core.class,"checked",MethodType.methodType(List.class,Object.class,Object.class)).invokeExact(values,seed);
    }
    @SuppressWarnings("unchecked") public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final RuntimeException runtime=new IllegalArgumentException("owned runtime");final Error error=new Error("owned error");final IOException checked=new IOException("owned checked");Trace.failure=new IllegalArgumentException("owned stage");
        for(int attempt=0;attempt<2;attempt++) {
            final Value seed=mode==2?null:new Value();final Object sentinel=new Object(),payload=new Object(),rawValue=new Object();
            final List values=mode==1?null:new ArrayList();if(values!=null)values.add(rawValue);
            final Object targetValue=mode==11?rawValue:seed;final Object element=mode==6?rawValue:new Target(targetValue);
            final List desired=mode==3?null:new Result();if(desired!=null)desired.add(element);
            final Object input=mode==7&&operation==1?new Object():values;
            Trace.events=0;Trace.values=null;Trace.seed=sentinel;Trace.result=null;Trace.payload=sentinel;Trace.failAt=mode==9?1:mode==10?2:0;
            final int actions=Trace.actions,reads=Trace.reads;
            Trace.action=new Runnable(){public void run(){Trace.actions++;require(Trace.events==1&&Trace.values==values&&Trace.seed==seed&&Trace.payload==sentinel);Trace.payload=payload;Trace.result=desired;if(values!=null)values.set(0,payload);if(mode==4)throw runtime;if(mode==5)throw error;if(mode==8)NestedSeedTransferHarness.<RuntimeException>fail(checked);}};
            List result=null;Target target=null;Value value=null;Throwable failure=null;try{result=call(route,operation,input,seed);target=(Target)result.get(0);value=(Value)target.value;}catch(Throwable caught){failure=caught;}
            final boolean bad=mode==7&&operation==1,entered=!bad,action=entered&&mode!=9,returned=action&&mode!=4&&mode!=5&&mode!=8&&mode!=10;
            require(Trace.events==(bad?0:mode==9||mode==4||mode==5||mode==8?1:12));
            require(Trace.actions==actions+(action?1:0)&&Trace.reads==reads+(returned&&desired!=null?1:0));
            Throwable wanted=mode==4?runtime:mode==5?error:mode==8?checked:mode==9||mode==10?Trace.failure:null;
            if(wanted!=null)require(failure==wanted&&result==null);else if(bad)require(failure instanceof ClassCastException);else if(mode==3)require(failure instanceof NullPointerException&&result==null);else if(mode==6)require(failure instanceof ClassCastException&&result==desired&&target==null);else if(mode==11)require(failure instanceof ClassCastException&&result==desired&&target==element&&value==null);else require(failure==null&&result==desired&&target==element&&value==seed);
            require(Trace.values==(action?values:null)&&Trace.seed==(action?seed:sentinel)&&Trace.payload==(action?payload:sentinel)&&Trace.result==(action?desired:null));
            if(values!=null)require(values.get(0)==(action?payload:rawValue));if(mode!=6)require(((Target)element).value==targetValue);
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
