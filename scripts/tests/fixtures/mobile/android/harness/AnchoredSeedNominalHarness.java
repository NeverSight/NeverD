import fixture.*;
import java.io.IOException;
public final class AnchoredSeedNominalHarness {
    private static void require(boolean ok) { if(!ok) throw new AssertionError("owned anchored seed nominal"); }
    private static final class Future extends Implementation {
        @Override public <Z> Provider accept(Token<Z> token,Z value) throws IOException { Trace.overrides++; return super.accept(token,value); }
    }
    @SuppressWarnings({"unchecked","rawtypes"})
    private static Provider call(int route,int operation,Token token,Object value) throws IOException {
        if(operation==2) return route==0?Core.discard(token,value):AnchoredSeedNominalCaller.discard(token,value);
        if(route==0) return Core.call(token,value);
        if(route==1) return AnchoredSeedNominalCaller.generic(token,value);
        if(route==2) return AnchoredSeedNominalCaller.checked(token,value);
        return AnchoredSeedNominalCaller.typed(token,(String)value);
    }
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Object value=mode==1?null:mode==2?Integer.valueOf(7):new String("owned-key");
        final Token<String> token=mode==12?null:new Token<String>() {};
        final RuntimeException runtime=new IllegalArgumentException("owned runtime");
        final Error error=new Error("owned error"); final IOException checked=new IOException("owned checked");
        for(int attempt=0;attempt<2;attempt++) {
            final Provider receiver=operation==0?new Implementation():new Future();
            final Provider returned=new Implementation(),replacement=new Implementation(),discarded=new Implementation();
            final Object sentinel=new Object(),payload=new Object(),prePayload=new Object();
            Trace.events=0;Trace.source=mode==3?null:receiver;Trace.result=mode==10?null:mode==11?receiver:returned;
            Trace.sentinelResult=discarded;Trace.failure=mode==8?checked:null;
            Trace.actualReceiver=null;Trace.captured=sentinel;Trace.token=sentinel;Trace.payload=sentinel;Trace.prePayload=sentinel;
            final int receivers=Trace.receivers,calls=Trace.calls,overrides=Trace.overrides,preCalls=Trace.preCalls,actions=Trace.actions;
            Trace.before=new Runnable(){public void run(){Trace.preCalls++;require(Trace.events==1&&Trace.actualReceiver==null&&Trace.captured==sentinel&&Trace.token==sentinel&&Trace.payload==sentinel);Trace.prePayload=prePayload;if(mode==4)throw runtime;if(mode==5)throw error;}};
            Trace.action=new Runnable(){public void run(){Trace.actions++;require(Trace.events==12&&Trace.actualReceiver==receiver&&Trace.captured==value&&Trace.token==token&&Trace.payload==sentinel);Trace.payload=payload;if(mode==9){Trace.result=replacement;Trace.source=returned;}if(mode==6)throw runtime;if(mode==7)throw error;}};
            Provider result=null;String text=null;Throwable failure=null;
            try { result=call(route,operation,token,value); if(operation!=2){text=(String)Trace.captured;require(text.length()==9);if(mode==10)result.toString();} } catch(Throwable caught){failure=caught;}
            final boolean early=mode==2&&route>=2&&operation!=2;
            final boolean body=!early&&mode!=3&&mode!=4&&mode!=5,completed=body&&mode!=6&&mode!=7&&mode!=8;
            final long events=early?0:body?completed?123:12:1;
            require(Trace.events==events&&Trace.receivers==receivers+(early?0:1)&&Trace.preCalls==preCalls+(early?0:1)&&Trace.calls==calls+(body?1:0)&&Trace.overrides==overrides+(body&&operation!=0?1:0)&&Trace.actions==actions+(body?1:0));
            require(Trace.prePayload==(early?sentinel:prePayload)&&Trace.payload==(body?payload:sentinel)&&Trace.actualReceiver==(body?receiver:null)&&Trace.captured==(body?value:sentinel)&&Trace.token==(body?token:sentinel));
            Throwable expected=mode==4||mode==6?runtime:mode==5||mode==7?error:mode==8?checked:null;
            if(expected!=null) require(failure==expected&&result==null);
            else if(mode==3) require(failure instanceof NullPointerException&&result==null);
            else if(operation==2) require(failure==null&&result==discarded);
            else if(mode==2) require(failure instanceof ClassCastException&&(early?result==null:result==returned));
            else if(mode==1||mode==10) require(failure instanceof NullPointerException&&(mode==1?result==returned:result==null));
            else require(failure==null&&result==(mode==9?replacement:mode==11?receiver:returned)&&text==value);
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
