import fixture.*;
public class FixedCompletionFactoryHarness {
    static void require(boolean value){if(!value)throw new AssertionError("owned fixed completion factory");}
    public static void main(String[]args)throws Throwable {
        int route=Integer.parseInt(args[0]),kind=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.calledKind=0;Trace.blockSeen=null;Trace.receiverSeen=null;Trace.completionSeen=null;Trace.callbackReceiver=null;Trace.callbackCompletion=null;Trace.delivered=null;Trace.callbackAnswer=null;
            int fail=mode>=4&&mode<16?(mode-4)%6+1:mode==16?3:mode==17?4:0;
            Throwable requested=mode>=10&&mode<16?new AssertionError("owned error"):new IllegalStateException("owned runtime");
            boolean run=mode!=16,complete=mode!=16&&mode!=17&&mode!=19;
            Trace.fail=fail;Trace.failure=requested;Trace.run=run;Trace.complete=complete;
            FixedCompletionFactoryProbe dual=attempt==0?new FixedCompletionFactoryProbe():new FixedCompletionFactoryProbe.Child();
            Object block=mode==18?null:dual,receiver=mode%4==0?null:mode%4==1?dual:mode%4==2?new Object[]{dual}:new Object();
            Completion completion=mode==19||mode==20?null:mode%2==0?dual:new FixedCompletionFactoryProbe.Child();
            Completion initial=mode%3==0?null:mode%3==1?completion:new FixedCompletionFactoryProbe();
            Completion callback=mode%3==1?null:mode%3==2?completion:new FixedCompletionFactoryProbe.Child();
            Completion terminal=new FixedCompletionFactoryProbe();Object payload=new Object(),answer=mode%4==0?new Object():mode%4==1?null:mode%4==2?receiver:dual;
            Trace.result=initial;Trace.callbackReplacement=callback;Trace.finalReplacement=terminal;Trace.payload=payload;Trace.answer=answer;
            Completion result=null;Throwable failure=null;
            try{result=FixedCompletionFactoryCaller.call(route,kind,block,receiver,completion);}catch(Throwable error){failure=error;}
            long events=0;boolean called=false,delivered=false,answered=false,finished=false;Throwable expectedFailure=null;
            for(int step=1;step<=6;step++) {
                if(step==3&&!run)continue;
                if(step==4&&(!run||!complete))continue;
                if(step==3&&block==null){require(failure instanceof NullPointerException);expectedFailure=failure;break;}
                if(step==4&&completion==null){require(failure instanceof NullPointerException);expectedFailure=failure;break;}
                if(step==3)called=true;if(step==4)delivered=true;
                if(step==5&&run)answered=true;
                events=events*10+step;
                if(step==fail){expectedFailure=requested;break;}
                if(step==6)finished=true;
            }
            boolean reached6=events%10==6;
            require(Trace.fail==fail&&Trace.failure==requested&&Trace.run==run&&Trace.complete==complete);
            require(Trace.calledKind==(called?kind+1:0)&&failure==expectedFailure&&Trace.events==events&&Trace.blockSeen==block&&Trace.receiverSeen==(kind==0?null:receiver)&&Trace.completionSeen==completion);
            require(Trace.callbackReceiver==(called&&kind==1?receiver:null)&&Trace.callbackCompletion==(called?completion:null)&&Trace.delivered==(delivered?payload:null));
            require(Trace.callbackAnswer==(answered?answer:null)&&Trace.result==(reached6?terminal:called?callback:initial));
            require(Trace.callbackReplacement==callback&&Trace.finalReplacement==terminal&&Trace.answer==answer&&Trace.payload==payload);
            require(result==(finished?(called?callback:initial):null));
            System.out.println("route="+route+",kind="+kind+",mode="+mode+",attempt="+attempt+",events="+Trace.events);
        }
    }
}
