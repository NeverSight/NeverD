import fixture.*;
import kotlin.jvm.functions.Function2;
import kotlin.coroutines.Continuation;
import kotlin.coroutines.intrinsics.IntrinsicsKt;
public class ScopedSuspendForwarderHarness {
    static void require(boolean value){if(!value)throw new AssertionError("owned scoped suspend forwarder");}
    public static void main(String[]args)throws Throwable {
        int route=Integer.parseInt(args[0]),kind=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        long[] longs={Long.MIN_VALUE,Long.MAX_VALUE,0L,-1L,0x1234567887654321L,0xfedcba9876543210L,0x100000000L,0xffffffffL};
        long[] doubles={0x7ff8000000001234L,0xfff8000000005678L,0L,Long.MIN_VALUE,0x7ff0000000000000L,0xfff0000000000000L,1L,0x7fefffffffffffffL};
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.bits=0;Trace.ownerSeen=null;Trace.contextSeen=null;Trace.callbackOwner=null;Trace.completionSeen=null;Trace.resumedValue=null;
            Trace.fail=mode>=4&&mode<16?(mode-4)%6+1:mode==16||mode==17?5:0;
            Trace.failure=mode>=10&&mode<=15||mode==17?new AssertionError("owned error"):new IllegalStateException("owned runtime");
            Trace.resume=mode!=16&&mode!=17&&mode!=18;
            Scope owner=attempt==0?new ScopedSuspendForwarderProbe():new ScopedSuspendForwarderProbe.Child();
            Object context=mode%3==0?null:new Object(),resumed=new Object();
            Trace.contextValue=context;Trace.resumeValue=resumed;
            Object expected=mode%4==0?new Object():mode%4==1?null:mode%4==2?IntrinsicsKt.getCOROUTINE_SUSPENDED():owner;
            Trace.result=expected;
            long bits=(kind==0?longs:doubles)[(mode+attempt)%8];
            Function2 block=mode==19?null:new ScopedSuspendForwarderProbe.Block();
            Continuation completion=mode==18||mode==20?null:new ScopedSuspendForwarderProbe.Completion();
            Object result=null;Throwable failure=null;
            try{result=ScopedSuspendForwarderCaller.call(route,kind,owner,bits,block,completion);}catch(Throwable error){failure=error;}
            long events=0;boolean captured=false,called=false,didResume=false,done=false;Throwable expectedFailure=null;
            for(int step=1;step<=6;step++) {
                if(step==4&&mode==19){expectedFailure=failure;require(failure instanceof NullPointerException);break;}
                if(step==5&&!Trace.resume)continue;
                if(step==5&&completion==null){expectedFailure=failure;require(failure instanceof NullPointerException);break;}
                if(step==2)captured=true;if(step==4)called=true;if(step==5)didResume=true;
                events=events*10+step;
                if(step==Trace.fail){expectedFailure=Trace.failure;break;}
                if(step==6)done=true;
            }
            require(failure==expectedFailure&&Trace.events==events);
            require(Trace.ownerSeen==(captured?owner:null)&&Trace.contextSeen==(captured?context:null)&&Trace.bits==(captured?bits:0));
            require(Trace.callbackOwner==(called?owner:null)&&Trace.completionSeen==(called?completion:null));
            require(Trace.resumedValue==(didResume?resumed:null)&&result==(done?expected:null));
            require(Trace.contextValue==context&&Trace.resumeValue==resumed&&Trace.result==expected);
            System.out.println("route="+route+",kind="+kind+",mode="+mode+",attempt="+attempt+",events="+Trace.events+",bits="+Long.toHexString(Trace.bits));
        }
    }
}
