import fixture.*;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.util.HashMap;
import java.util.Map;
public final class ComparableMapTransferHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned comparable map transfer");}
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure)throws X{throw (X)failure;}
    private static final class Key implements Comparable<Key> {
        public int compareTo(Key other){Trace.comparisons++;throw new AssertionError("owned unwanted comparison");}
    }
    private static final class Result extends HashMap<String,Object> {
        public Object get(Object key){Trace.reads++;require(Trace.events==12);return super.get(key);}
    }
    private static Map call(int route,int operation,Object input,boolean reverse)throws Throwable {
        if(route==0)return operation==0?Core.call((Map)input,reverse):Core.checked(input,reverse);
        if(route==1)return operation==0?ComparableMapTransferCaller.call((Map)input,reverse):ComparableMapTransferCaller.checked(input,reverse);
        if(route==2){try{return (Map)Core.class.getMethod(operation==0?"call":"checked",operation==0?Map.class:Object.class,boolean.class).invoke(null,input,reverse);}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (Map)MethodHandles.publicLookup().findStatic(Core.class,"call",MethodType.methodType(Map.class,Map.class,boolean.class)).invokeExact((Map)input,reverse);
        return (Map)MethodHandles.publicLookup().findStatic(Core.class,"checked",MethodType.methodType(Map.class,Object.class,boolean.class)).invokeExact(input,reverse);
    }
    @SuppressWarnings("unchecked") public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final RuntimeException runtime=new IllegalArgumentException("owned runtime");final Error error=new Error("owned error");final IOException checked=new IOException("owned checked");Trace.failure=new IllegalArgumentException("owned stage");
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),payload=new Object(),unbounded=new Object();final Key key=new Key();
            final Map source=mode==1?null:new HashMap();if(source!=null&&mode!=2)source.put("owned-initial",unbounded);
            final Map desired=mode==3?null:new Result();if(desired!=null)desired.put("owned-result",mode==11?unbounded:key);
            final Object input=mode==7&&operation==1?new Object():source;final boolean reverse=(mode+attempt)%2==0;
            Trace.events=0;Trace.input=null;Trace.reverse=!reverse;Trace.result=null;Trace.payload=sentinel;Trace.failAt=mode==9?1:mode==10?2:0;
            final int actions=Trace.actions,comparisons=Trace.comparisons,reads=Trace.reads;
            Trace.action=new Runnable(){public void run(){Trace.actions++;require(Trace.events==1&&Trace.input==source&&Trace.reverse==reverse&&Trace.payload==sentinel);Trace.payload=payload;Trace.result=desired;if(source!=null)source.put("owned-callback",payload);if(mode==4)throw runtime;if(mode==5)throw error;if(mode==8)ComparableMapTransferHarness.<RuntimeException>fail(checked);}};
            Map result=null;Key value=null;Throwable failure=null;try{result=call(route,operation,input,reverse);value=(Key)result.get("owned-result");}catch(Throwable caught){failure=caught;}
            final boolean bad=mode==7&&operation==1,entered=!bad,captured=entered&&mode!=9,action=captured,returned=action&&mode!=4&&mode!=5&&mode!=8&&mode!=10;
            require(Trace.events==(bad?0:mode==9||mode==4||mode==5||mode==8?1:12));
            require(Trace.actions==actions+(action?1:0)&&Trace.comparisons==comparisons&&Trace.reads==reads+(returned&&desired!=null?1:0));
            Throwable wanted=mode==4?runtime:mode==5?error:mode==8?checked:mode==9||mode==10?Trace.failure:null;
            if(wanted!=null)require(failure==wanted&&result==null);else if(bad)require(failure instanceof ClassCastException);else if(mode==3)require(failure instanceof NullPointerException&&result==null);else if(mode==11)require(failure instanceof ClassCastException&&result==desired&&value==null);else require(failure==null&&result==desired&&value==key);
            require(Trace.input==(captured?source:null)&&Trace.reverse==(captured?reverse:!reverse)&&Trace.payload==(action?payload:sentinel)&&Trace.result==(action?desired:null));
            if(source!=null){require(source.containsKey("owned-callback")==action);if(mode!=2)require(source.get("owned-initial")==unbounded);}
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
