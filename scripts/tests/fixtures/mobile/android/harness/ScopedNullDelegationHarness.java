import fixture.*;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ScopedNullDelegationHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned scoped null delegation");}
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure)throws X{throw (X)failure;}
    @SuppressWarnings({"unchecked","rawtypes"})
    private static Link<?> call(int route,int operation,Object first,Object second,Peer<?> next)throws Throwable {
        if(route==0)return operation==0?new Link(first,(Peer)next):new Link(first,second,(Peer)next);
        if(route==1)return operation==0?ScopedNullDelegationCaller.shorter(first,(Peer)next):ScopedNullDelegationCaller.longer(first,second,(Peer)next);
        if(route==2){try{return operation==0?Link.class.getConstructor(Object.class,Peer.class).newInstance(first,next):Link.class.getConstructor(Object.class,Object.class,Peer.class).newInstance(first,second,next);}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (Link)MethodHandles.publicLookup().findConstructor(Link.class,MethodType.methodType(void.class,Object.class,Peer.class)).invokeExact(first,(Peer)next);
        return (Link)MethodHandles.publicLookup().findConstructor(Link.class,MethodType.methodType(void.class,Object.class,Object.class,Peer.class)).invokeExact(first,second,(Peer)next);
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final RuntimeException runtime=new IllegalArgumentException("owned runtime");final Error error=new Error("owned error");final IOException checked=new IOException("owned checked");Trace.failure=new IllegalArgumentException("owned stage");
        for(int attempt=0;attempt<2;attempt++) {
            Trace.failAt=0;Trace.action=new Runnable(){public void run(){}};final Peer<String> next=mode==1?null:new Link<String>(null,null,null,true,false);
            final Object first=mode==2?null:mode==10?Integer.valueOf(7):new String("owned-first"),second=mode==3?first:new Object(),wantedSecond=operation==0?null:second,sentinel=new Object(),payload=new Object();
            Trace.events=0;Trace.link=null;Trace.payload=sentinel;Trace.failAt=mode>=6&&mode<=9?mode-5:0;final int actions=Trace.actions;
            Trace.action=new Runnable(){public void run(){Trace.actions++;require(Trace.events==1234&&Trace.link!=null&&Trace.link.first==first&&Trace.link.second==wantedSecond&&Trace.link.next==next&&Trace.link.active&&!Trace.link.closed&&Trace.payload==sentinel);Trace.payload=payload;if(mode==4)throw runtime;if(mode==5)throw error;if(mode==11)ScopedNullDelegationHarness.<RuntimeException>fail(checked);}};
            Link<?> result=null;String value=null;Throwable failure=null;try{result=call(route,operation,first,second,next);require(result.iterator()==result&&result.hasNext());value=(String)result.next();require(value.length()==11);}catch(Throwable caught){failure=caught;}
            final boolean callback=mode<6||mode>9;final long events=mode==6?1:mode==7?12:mode==8?123:mode==4||mode==5||mode==9||mode==11?1234:operation==0?1234567:123456;
            require(Trace.events==events&&Trace.actions==actions+(callback?1:0)&&Trace.payload==(callback?payload:sentinel));
            Throwable wanted=mode==4?runtime:mode==5?error:mode>=6&&mode<=9?Trace.failure:mode==11?checked:null;
            if(wanted!=null)require(failure==wanted&&result==null);
            else if(mode==2)require(failure instanceof NullPointerException&&result==Trace.link&&value==null);
            else if(mode==10)require(failure instanceof ClassCastException&&result==Trace.link);
            else require(failure==null&&result==Trace.link&&value==first);
            if(mode==6)require(Trace.link==null);else {require(Trace.link!=null&&Trace.link.first==first&&Trace.link.second==(mode==7?null:wantedSecond)&&Trace.link.next==(mode==7||mode==8?null:next));require(Trace.link.active==callback&&!Trace.link.closed);}
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
