import fixture.*;
import java.io.IOException;
public final class MethodSeedBridgeHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned method seed bridge");}
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure)throws X{throw (X)failure;}
    private static final class Future extends Provider {
        @Override public <Z> Provider accept(Z value,boolean left,boolean right){Trace.overrides++;Trace.hit(9);return super.accept(value,left,right);}
    }
    private static Closed call(int route,Object value,boolean left,boolean right){
        if(route==0)return Core.call(value,left,right);
        if(route==1)return MethodSeedBridgeCaller.generic(value,left,right);
        if(route==2)return MethodSeedBridgeCaller.checked(value,left,right);
        return MethodSeedBridgeCaller.typed((String)value,left,right);
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Object value=mode==1?null:mode==2?Integer.valueOf(7):new String("owned-key");
        final RuntimeException runtime=new IllegalArgumentException("owned runtime");final Error error=new Error("owned error");final IOException checked=new IOException("owned checked");
        for(int attempt=0;attempt<2;attempt++) {
            final boolean left=((mode+attempt)&1)!=0,right=((mode+attempt)&2)!=0;
            final Provider receiver=operation==0?new Provider():new Future(),returned=new Provider(),replacement=new Provider();
            final Closed broad=new Closed();final Middle middle=new Middle();
            final Object sentinel=new Object(),payload=new Object(),prePayload=new Object();
            Trace.events=0;Trace.left=!left;Trace.right=!right;Trace.source=mode==3?null:receiver;
            Trace.result=mode==10?null:mode==11?receiver:mode==12?broad:mode==13?middle:returned;
            Trace.actualReceiver=null;Trace.captured=sentinel;Trace.payload=sentinel;Trace.prePayload=sentinel;
            final int receivers=Trace.receivers,calls=Trace.calls,overrides=Trace.overrides,preCalls=Trace.preCalls,actions=Trace.actions;
            final int middleCalls=Trace.middleCalls,leafCalls=Trace.leafCalls,middleAfter=Trace.middleAfter,leafAfter=Trace.leafAfter;
            Trace.before=new Runnable(){public void run(){Trace.preCalls++;require(Trace.events==7&&Trace.actualReceiver==null&&Trace.captured==sentinel&&Trace.left==!left&&Trace.right==!right&&Trace.payload==sentinel);Trace.prePayload=prePayload;if(mode==4)throw runtime;if(mode==5)throw error;}};
            Trace.action=new Runnable(){public void run(){Trace.actions++;require(Trace.events==(operation==0?7123:79123)&&Trace.actualReceiver==receiver&&Trace.captured==value&&Trace.left==left&&Trace.right==right&&Trace.payload==sentinel);Trace.payload=payload;if(mode==9){Trace.result=replacement;Trace.source=returned;}if(mode==14)Trace.result=broad;if(mode==15)Trace.result=middle;if(mode==6)throw runtime;if(mode==7)throw error;if(mode==8)MethodSeedBridgeHarness.<RuntimeException>fail(checked);}};
            Closed result=null;String text=null;Throwable failure=null;
            try{result=call(route,value,left,right);text=(String)Trace.captured;require(text.length()==9);if(mode==10)result.toString();}catch(Throwable caught){failure=caught;}
            final boolean early=mode==2&&route>=2,body=!early&&mode!=3&&mode!=4&&mode!=5,baseDone=body&&mode!=6&&mode!=7&&mode!=8;
            final boolean middleDone=baseDone&&mode!=12&&mode!=14,leafDone=middleDone&&mode!=13&&mode!=15;
            long events=early?0:7;if(body){if(operation==1)events=events*10+9;events=events*1000+123;}if(baseDone)events=events*10+4;if(middleDone)events=events*10+5;if(leafDone)events=events*10+6;
            require(Trace.events==events&&Trace.receivers==receivers+(early?0:1)&&Trace.preCalls==preCalls+(early?0:1)&&Trace.calls==calls+(body?1:0)&&Trace.overrides==overrides+(body&&operation==1?1:0)&&Trace.actions==actions+(body?1:0));
            require(Trace.middleCalls==middleCalls+(body?1:0)&&Trace.leafCalls==leafCalls+(body?1:0)&&Trace.middleAfter==middleAfter+(middleDone?1:0)&&Trace.leafAfter==leafAfter+(leafDone?1:0));
            require(Trace.prePayload==(early?sentinel:prePayload)&&Trace.payload==(body?payload:sentinel)&&Trace.actualReceiver==(body?receiver:null)&&Trace.captured==(body?value:sentinel));
            require(Trace.left==(body?left:!left)&&Trace.right==(body?right:!right));
            Throwable expected=mode==4||mode==6?runtime:mode==5||mode==7?error:mode==8?checked:null;
            if(expected!=null)require(failure==expected&&result==null);
            else if(mode==2)require(failure instanceof ClassCastException&&(early?result==null:result==returned));
            else if(mode>=12)require(failure instanceof ClassCastException&&result==null);
            else if(mode==1||mode==3||mode==10)require(failure instanceof NullPointerException&&(mode==1?result==returned:result==null));
            else require(failure==null&&result==(mode==9?replacement:mode==11?receiver:returned)&&text==value);
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
