import fixture.Client;
import fixture.Provider;
import fixture.Trace;
import fixture.Failure;
import fixture.Value;


import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ClosedClassSeedThrowsHarness {
    private static void require(boolean value){if(!value)throw new AssertionError("owned closed class seed throws");}
    private static final class Tagged extends Value {int tag(){return 47;}}
    private static final class Derived extends Provider {@Override public Value read(Value values) throws Failure {Trace.dispatch++;return super.read(values);}}
    @SuppressWarnings({"rawtypes","unchecked"})
    private static Value call(int route,int operation,Provider receiver,Value values,long count,boolean flag)throws Throwable {
        if(route==0)return operation==0?Client.call(receiver,values):Client.full(receiver,values,count,flag);
        if(route==1)return operation==0?ClosedClassSeedThrowsCaller.call(receiver,values):ClosedClassSeedThrowsCaller.full(receiver,values,count,flag);
        if(route==2){try{return operation==0?(Value)Client.class.getMethod("call",Provider.class,Value.class).invoke(null,receiver,values):(Value)Client.class.getMethod("full",Provider.class,Value.class,long.class,boolean.class).invoke(null,receiver,values,count,flag);}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (Value)MethodHandles.publicLookup().findStatic(Client.class,"call",MethodType.methodType(Value.class,Provider.class,Value.class)).invokeExact(receiver,values);
        return (Value)MethodHandles.publicLookup().findStatic(Client.class,"full",MethodType.methodType(Value.class,Provider.class,Value.class,long.class,boolean.class)).invokeExact(receiver,values,count,flag);
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Provider receiver=mode==1?null:mode==10?new Derived():new Provider();final Value values=mode==2?null:mode==3?new Value():new Tagged();if(values!=null)values.blocked=mode==3;final long count=mode%2==0?Long.MIN_VALUE+mode:Long.MAX_VALUE-mode;final boolean flag=mode%2==0;
        final Object desired=mode==8?new Value():mode==9?null:mode==11?new String("owned wrong root"):new Tagged();
        final Failure actionFailure=new Failure("owned action failure");final Error actionError=new Error("owned action error");Trace.failure=new Failure("owned stage failure");Trace.failAt=mode==6?1:mode==7?2:0;
        for(int attempt=0;attempt<2;attempt++){
            if(values!=null&&mode!=3)values.clear();final Object sentinel=new Object(),payload=new Object();final Value initial=new Tagged();Trace.events=0;Trace.receiver=sentinel;Trace.values=null;Trace.flag=!(operation==0?false:flag);Trace.count=0;Trace.payload=sentinel;Trace.result=initial;int calls=Trace.calls,dispatch=Trace.dispatch;
            Trace.action=new fixture.Action(){public void run() throws Failure {require(Trace.events==1&&Trace.receiver==receiver&&Trace.values==values&&Trace.flag==(operation==0?false:flag)&&Trace.count==(operation==0?17L:count)&&Trace.payload==sentinel);Trace.hit(3);Trace.payload=payload;Trace.result=desired;if(values!=null)values.add("owned");if(mode==4)throw actionFailure;if(mode==5)throw actionError;}};
            Value actual=initial;int tag=-7;Throwable failure=null;try{actual=call(route,operation,receiver,values,count,flag);tag=((Tagged)actual).tag();}catch(Throwable error){failure=error;}
            if(mode==1&&operation==0){require(failure instanceof NullPointerException&&actual==initial&&tag==-7&&Trace.calls==calls&&Trace.dispatch==dispatch&&Trace.events==0&&Trace.receiver==sentinel&&Trace.values==null&&Trace.payload==sentinel&&Trace.result==initial);}
            else {
                require(Trace.calls==calls+1&&Trace.dispatch==dispatch+(mode==10&&operation==0?1:0)&&Trace.receiver==receiver&&Trace.values==values&&Trace.flag==(operation==0?false:flag)&&Trace.count==(operation==0?17L:count)&&Trace.events==(mode==6?1:mode==3||mode==4||mode==5?13:132));
                Throwable expected=mode==4?actionFailure:mode==5?actionError:mode==6||mode==7?Trace.failure:null;
                if(expected!=null)require(failure==expected&&actual==initial&&tag==-7);
                else if(mode==3)require(failure instanceof UnsupportedOperationException&&actual==initial&&tag==-7);
                else if(mode==8)require(failure instanceof ClassCastException&&actual==desired&&tag==-7);
                else if(mode==9)require(failure instanceof NullPointerException&&actual==null&&tag==-7);
                else if(mode==11)require(failure instanceof ClassCastException&&actual==initial&&tag==-7);
                else require(failure==null&&actual==desired&&tag==47);
                require(Trace.payload==(mode==6?sentinel:payload)&&Trace.result==(mode==6?initial:desired));
                if(values!=null)require(values.size()==(mode==3||mode==6?0:1));
            }
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
