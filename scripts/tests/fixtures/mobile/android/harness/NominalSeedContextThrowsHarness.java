import fixture.*;
import java.io.IOException;
public final class NominalSeedContextThrowsHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned nominal seed context throws");}
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure)throws X{throw (X)failure;}
    private static final class Future extends Provider {
        @Override public <Z> Context read(String context,Z value,boolean left,boolean right)throws IOException {Trace.overrides++;Trace.hit(9);return super.read(context,value,left,right);}
    }
    private static Context call(int route,Object value,boolean left,boolean right)throws IOException {
        if(route==0)return Core.call(value,left,right);
        if(route==1)return NominalSeedContextThrowsCaller.generic(value,left,right);
        if(route==2)return NominalSeedContextThrowsCaller.checked(value,left,right);
        return NominalSeedContextThrowsCaller.typed((Context)value,left,right);
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final RuntimeException runtime=new IllegalArgumentException("owned runtime");final Error error=new Error("owned error");final IOException checked=new IOException("owned checked");
        java.lang.reflect.Method member=Provider.class.getMethod("read",String.class,Object.class,boolean.class,boolean.class);
        require(member.getTypeParameters().length==1&&member.getTypeParameters()[0].getBounds()[0]==Object.class&&member.getGenericParameterTypes()[0]==String.class&&member.getGenericParameterTypes()[1].equals(member.getTypeParameters()[0])&&member.getGenericReturnType()==Context.class&&member.getExceptionTypes()[0]==IOException.class);
        for(int attempt=0;attempt<2;attempt++) {
            final Provider receiver=operation==0?new Provider():new Future(),replacement=operation==0?new Provider():new Future(),actual=mode==15?replacement:receiver;
            final Context seed=new Context(),initial=mode==11?null:new Context(),changed=new Context(),sentinelResult=new Context();
            final Object value=mode==1?null:mode==2?Integer.valueOf(7):seed;
            final String context=mode==3?null:new String("owned-context"),otherContext=new String("changed-context"),sentinelContext=new String("sentinel-context");
            final Object sentinel=new Object(),prePayload=new Object();final String payload=new String("owned-key");
            final boolean left=((route&1)!=0)^(attempt!=0),right=(route&2)!=0;
            Trace.events=0;Trace.source=mode==4?null:receiver;Trace.result=initial;Trace.nextContext=context;
            Trace.actualReceiver=null;Trace.captured=sentinel;Trace.context=sentinelContext;Trace.observed=-31;Trace.left=!left;Trace.right=!right;Trace.payload=sentinel;Trace.prePayload=sentinel;
            final int receivers=Trace.receivers,calls=Trace.calls,overrides=Trace.overrides,preCalls=Trace.preCalls,actions=Trace.actions;
            Trace.before=new Runnable(){public void run(){Trace.preCalls++;require(Trace.events==7&&Trace.actualReceiver==null&&Trace.captured==sentinel&&Trace.context==sentinelContext&&Trace.observed==-31&&Trace.payload==sentinel&&Trace.left!=left&&Trace.right!=right);Trace.prePayload=prePayload;if(mode==14)Trace.nextContext=otherContext;if(mode==15)Trace.source=replacement;if(mode==5)throw runtime;if(mode==6)throw error;}};
            Trace.action=new Runnable(){public void run(){Trace.actions++;require(Trace.events==(operation==0?712:7912)&&Trace.actualReceiver==actual&&Trace.captured==value&&Trace.context==context&&Trace.observed==13&&Trace.left==left&&Trace.right==right&&Trace.payload==sentinel);Trace.payload=mode==1?null:mode==2?value:payload;if(mode==10){Trace.result=changed;Trace.source=replacement;}if(mode==12)Trace.result=seed;if(mode==13)Trace.result=null;if(mode==7)throw runtime;if(mode==8)throw error;if(mode==9)NominalSeedContextThrowsHarness.<RuntimeException>fail(checked);}};
            Context result=sentinelResult;String text=null;Throwable failure=null;
            try{result=call(route,value,left,right);text=(String)Trace.payload;require(text.length()==9);}catch(Throwable caught){failure=caught;}
            final boolean early=mode==2&&route>=2,body=!early&&mode!=4&&mode!=5&&mode!=6,action=body&&mode!=3,done=action&&mode!=7&&mode!=8&&mode!=9;
            long events=early?0:7;if(body){if(operation==1)events=events*10+9;events=events*10+1;}if(action)events=events*10+2;if(done)events=events*10+3;
            require(Trace.events==events&&Trace.receivers==receivers+(early?0:1)&&Trace.preCalls==preCalls+(early?0:1)&&Trace.calls==calls+(body?1:0)&&Trace.overrides==overrides+(body&&operation==1?1:0)&&Trace.actions==actions+(action?1:0));
            require(Trace.prePayload==(early?sentinel:prePayload)&&Trace.payload==(action?(mode==1?null:mode==2?value:payload):sentinel)&&Trace.actualReceiver==(body?actual:null)&&Trace.captured==(body?value:sentinel)&&Trace.context==(body?context:sentinelContext));
            require(Trace.observed==(action?13:-31)&&Trace.left==(body?left:!left)&&Trace.right==(body?right:!right)&&Trace.nextContext==(mode==14?otherContext:context));
            Context expectedResult=mode==10?changed:mode==12?seed:mode==13?null:initial;require(result==(done?expectedResult:sentinelResult));
            Throwable expected=mode==5||mode==7?runtime:mode==6||mode==8?error:mode==9?checked:null;
            if(expected!=null)require(failure==expected);
            else if(mode==2)require(failure instanceof ClassCastException);
            else if(mode==1||mode==3||mode==4)require(failure instanceof NullPointerException);
            else require(failure==null&&text==payload);
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
