import fixture.Core;
import fixture.Mirror;
import fixture.Token;
import fixture.Trace;
import java.util.function.Function;
public final class NullaryBinaryFormalHarness {
    private static void check(boolean ok,String detail) {
        if(!ok)throw new AssertionError("owned nullary binary formal mismatch: "+detail);
    }
    @SuppressWarnings("unchecked")
    private static void attempt(final int route,final int mode) {
        final Token first=new Token(),late=new Token();
        final Mirror<Object,Object> other=value -> value;
        final Object invalid=new Object(),marker=new Object(),lateMarker=new Object(),applied=new Object();
        final Object source=mode==14?new Object():mode==15?null:new String("owned value");
        final RuntimeException runtime=new IllegalStateException("owned runtime");
        final Error error=new LinkageError("owned error");
        final int[] actions={0};final Object[] callback={null};
        Trace.calls=Trace.stage=Trace.applies=0;Trace.seen=null;Trace.applied=applied;Trace.marker=marker;
        Trace.result=mode==1?null:mode==2?invalid:mode==12?other:first;
        Trace.action=mode==3?null:() -> {
            actions[0]++;
            check(actions[0]==1 && Trace.calls==1 && Trace.stage==1 && Trace.applies==0,"original factory and action order");
            if(mode==4)throw runtime;
            if(mode==5)throw error;
            if(mode==6)Trace.result=late;
            if(mode==7)Trace.result=null;
            if(mode==8)Trace.result=invalid;
            if(mode==9)Trace.marker=lateMarker;
            if(mode==10)callback[0]=first.apply(source);
            if(mode==11)Trace.action=null;
            if(mode==13)Trace.result=other;
        };
        Object result=null;Throwable failure=null;
        try {
            switch(route) {
                case 0:result=Core.raw();break;
                case 1:result=Core.checked();break;
                case 2:result=Core.ignored();break;
                case 3:result=Core.relay();break;
                case 4:result=Core.standard();break;
                case 5:result=Core.applied(source);break;
                default:throw new AssertionError("owned route");
            }
        } catch(Throwable caught){failure=caught;}
        if(route>=4) {
            check(Trace.calls==0 && Trace.stage==0 && Trace.applies==0 && actions[0]==0,"standard call does not enter owned provider");
            if(route==4)check(failure==null && result instanceof Function && ((Function)result).apply(source)==source,"actual standard identity function");
            else if(mode==14)check(failure instanceof ClassCastException,"standard result cast remains late");
            else check(failure==null && result==source,"standard identity result reference");
            return;
        }
        boolean actionFailed=mode==3 || mode==4 || mode==5;
        check(Trace.calls==1 && actions[0]==(mode==3?0:1) && Trace.stage==(actionFailed?1:2)
                && Trace.applies==(mode==10?1:0) && Trace.seen==(mode==10?source:null)
                && callback[0]==(mode==10?applied:null),"exact calls, partial effects and original callback body");
        Object expected=mode==6?late:mode==7?null:mode==8?invalid:mode==13?other:mode==1?null:mode==2?invalid:mode==12?other:first;
        if(mode==3)check(failure instanceof NullPointerException,"null action failure");
        else if(mode==4)check(failure==runtime,"runtime identity");
        else if(mode==5)check(failure==error,"error identity");
        else if(expected==invalid || (route==1 && expected==other))check(failure instanceof ClassCastException,"original erased and checked casts");
        else check(failure==null && result==(route==2?mode==9?lateMarker:marker:expected),"opaque provider result and late marker");
    }
    public static void main(String[] args) {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int i=0;i<2;i++)attempt(route,mode);
        System.out.println("attempts=2,nullary-binary-formal=matched");
    }
}
