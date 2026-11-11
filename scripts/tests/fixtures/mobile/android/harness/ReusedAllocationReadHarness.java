import fixture.Provider;
import fixture.Value;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ReusedAllocationReadHarness {
    private static void require(boolean value){if(!value)throw new AssertionError("owned reused allocation read");}
    private static Value call(int route,int operation)throws Throwable {
        if(route==0)return operation==0?Provider.constant():operation==1?Provider.volatileRead():operation==2?Provider.distinct():Provider.cast();if(route==1)return operation==0?ReusedAllocationReadCaller.constant():operation==1?ReusedAllocationReadCaller.volatileRead():operation==2?ReusedAllocationReadCaller.distinct():ReusedAllocationReadCaller.cast();String name=operation==0?"constant":operation==1?"volatileRead":operation==2?"distinct":"cast";
        if(route==2){try{return (Value)Provider.class.getMethod(name).invoke(null);}catch(InvocationTargetException failure){throw failure.getCause();}}
        return (Value)MethodHandles.publicLookup().findStatic(Provider.class,name,MethodType.methodType(Value.class)).invokeExact();
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final String initial=mode==1?null:new String("mutable initial"),replacement=mode==9?null:new String("replacement"),middle=mode==2?null:new String("middle"),mutated=new String("after arguments");
        final String first=operation==1?initial:"owned-first",second=operation==0?"owned-shared":operation==1?middle:"owned-second",third=operation==0?second:operation==1?first:"owned-third";
        final RuntimeException actionFailure=new IllegalArgumentException("owned action failure");final Error actionError=new Error("owned action error");
        Trace.failure=new IllegalArgumentException("owned stage failure");Trace.failAt=mode==6?1:mode==7?2:0;Trace.replacement=replacement;Trace.middle=middle;
        Value previous=null;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),payload=new Object();Provider.field=initial;Trace.events=0;Trace.receiver=null;Trace.payload=sentinel;int calls=Trace.calls;
            Trace.action=mode==3?null:new Runnable(){public void run(){Value seen=(Value)Trace.receiver;require(Trace.events==1&&seen.first==first&&seen.second==second&&seen.third==third&&Trace.payload==sentinel);Trace.payload=payload;if(mode==10)Provider.field=mutated;if(mode==8)Trace.receiver=sentinel;if(mode==4)throw actionFailure;if(mode==5)throw actionError;}};
            Value actual=null;Throwable failure=null;try{actual=call(route,operation);}catch(Throwable error){failure=error;}
            require(Trace.calls==calls+1&&Trace.events==(mode==3||mode==4||mode==5||mode==6?1:12)&&Trace.targetInitializations==1&&Trace.changerInitializations==(operation==1?1:0));
            Throwable expected=mode==4?actionFailure:mode==5?actionError:mode==6||mode==7?Trace.failure:null;
            if(expected!=null)require(failure==expected&&actual==null);
            else if(mode==3)require(failure instanceof NullPointerException&&actual==null);
            else {require(failure==null&&actual!=null&&actual!=previous&&actual.first==first&&actual.second==second&&actual.third==third);previous=actual;}
            require(Trace.payload==(mode==3||mode==6?sentinel:payload));
            require(Provider.field==(mode==10?mutated:operation==1&&attempt==0?replacement:initial));
            if(mode==8)require(Trace.receiver==sentinel&&actual!=Trace.receiver);else {Value seen=(Value)Trace.receiver;require(seen.first==first&&seen.second==second&&seen.third==third);}
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
