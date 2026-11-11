import fixture.*;
import kotlin.jvm.functions.Function2;
import kotlin.coroutines.Continuation;
import kotlin.coroutines.intrinsics.IntrinsicsKt;
public class ScopedOwnerCompletionHarness {
    static void require(boolean value){if(!value)throw new AssertionError("owned scoped owner completion");}
    public static void main(String[]args)throws Throwable {
        int route=Integer.parseInt(args[0]),kind=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        long[] values={Long.MIN_VALUE,Long.MAX_VALUE,0L,-1L,0x1234567887654321L,0xfedcba9876543210L,0x100000000L,0xffffffffL};
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.bits=0;Trace.sessionSeen=null;Trace.outerSeen=null;Trace.tokenSeen=null;Trace.callbackOwner=null;Trace.completionSeen=null;Trace.resumedValue=null;
            Trace.fail=mode>=4&&mode<16?(mode-4)%6+1:mode==16||mode==17?5:0;
            Trace.failure=mode>=10&&mode<=15||mode==17?new AssertionError("owned error"):new IllegalStateException("owned runtime");
            Trace.resume=mode!=16&&mode!=17&&mode!=18;
            Object token=mode%3==0?null:new Object(),resumed=new Object();
            Core creator=new Core(token),owner=kind==0?creator:new Core(new Object());
            Object session=mode==20?null:mode==21?new Object():creator.openSession();
            Trace.otherScope=new Core(new Object()).openSession();Trace.resumeValue=resumed;
            Object expected=mode%4==0?new Object():mode%4==1?null:mode%4==2?IntrinsicsKt.getCOROUTINE_SUSPENDED():session;
            Trace.result=expected;
            long bits=values[(mode+attempt)%values.length];
            Function2 block=mode==19?null:attempt==0?new ScopedOwnerCompletionProbe.Block():new ScopedOwnerCompletionProbe.Child();
            Continuation completion=mode==18||mode==22?null:new ScopedOwnerCompletionProbe.Completion();
            Object result=null;Throwable failure=null;
            try{result=ScopedOwnerCompletionCaller.call(route,owner,session,bits,block,completion);}catch(Throwable error){failure=error;}
            long events=0;boolean captured=false,called=false,didResume=false,done=false;Throwable expectedFailure=null;
            for(int step=1;step<=6;step++) {
                if(step==2&&mode==20||step==4&&mode==19){expectedFailure=failure;require(failure instanceof NullPointerException);break;}
                if(step==2&&mode==21){expectedFailure=failure;require(failure instanceof ClassCastException);break;}
                if(step==5&&!Trace.resume)continue;
                if(step==5&&completion==null){expectedFailure=failure;require(failure instanceof NullPointerException);break;}
                if(step==2)captured=true;if(step==4)called=true;if(step==5)didResume=true;
                events=events*10+step;
                if(step==Trace.fail){expectedFailure=Trace.failure;break;}
                if(step==6)done=true;
            }
            require(failure==expectedFailure&&Trace.events==events);
            require(Trace.sessionSeen==(captured?session:null)&&Trace.outerSeen==(captured?creator:null)&&Trace.tokenSeen==(captured?token:null)&&Trace.bits==(captured?bits:0));
            require(Trace.callbackOwner==(called?session:null)&&Trace.completionSeen==(called?completion:null));
            require(Trace.resumedValue==(didResume?resumed:null)&&result==(done?expected:null));
            require(creator.token==token&&Trace.resumeValue==resumed&&Trace.result==expected);
            System.out.println("route="+route+",kind="+kind+",mode="+mode+",attempt="+attempt+",events="+Trace.events+",bits="+Long.toHexString(Trace.bits));
        }
    }
}
