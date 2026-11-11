import fixture.*;
import java.io.IOException;
public final class UnusedScalarThrowsHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned unused scalar throws");}
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure)throws X{throw (X)failure;}
    private static final class Future extends Provider {
        @Override public <Z> int read(Context value)throws IOException {Trace.overrides++;Trace.hit(9);return super.read(value)^Trace.mask;}
    }
    private static int call(int route,Object value)throws IOException {
        if(route==0)return Core.call(value);
        if(route==1)return UnusedScalarThrowsCaller.generic(value);
        if(route==2)return UnusedScalarThrowsCaller.checked(value);
        return UnusedScalarThrowsCaller.typed((Context)value);
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final RuntimeException runtime=new IllegalArgumentException("owned runtime");final Error error=new Error("owned error");final IOException checked=new IOException("owned checked");
        java.lang.reflect.Method member=Provider.class.getMethod("read",Context.class);
        require(member.getTypeParameters().length==1&&member.getTypeParameters()[0].getBounds()[0]==Object.class&&member.getGenericParameterTypes()[0]==Context.class&&member.getGenericReturnType()==int.class&&member.getExceptionTypes()[0]==IOException.class);
        for(int attempt=0;attempt<2;attempt++) {
            final Provider receiver=operation==0?new Provider():new Future(),replacement=operation==0?new Provider():new Future();
            final Provider actual=mode==14?replacement:receiver;
            final Context context=new Context(),sentinelContext=new Context();context.value=43+attempt;
            final int observed=context.value,initial=mode==10?Integer.MIN_VALUE:mode==11?Integer.MAX_VALUE:1237+attempt,changed=2039+attempt,mask=0x5A5A+attempt;
            final Object value=mode==1?null:mode==2?Integer.valueOf(7):context;
            final Object sentinel=new Object(),prePayload=new Object();final String payload=new String("owned-key");
            Trace.events=0;Trace.source=mode==3?null:receiver;Trace.result=initial;Trace.mask=mask;
            Trace.actualReceiver=null;Trace.captured=sentinelContext;Trace.observed=-31;Trace.payload=sentinel;Trace.prePayload=sentinel;
            final int receivers=Trace.receivers,calls=Trace.calls,overrides=Trace.overrides,preCalls=Trace.preCalls,actions=Trace.actions;
            Trace.before=new Runnable(){public void run(){Trace.preCalls++;require(Trace.events==7&&Trace.actualReceiver==null&&Trace.captured==sentinelContext&&Trace.observed==-31&&Trace.payload==sentinel);Trace.prePayload=prePayload;if(mode==14)Trace.source=replacement;if(mode==15)Trace.result=changed;if(mode==4)throw runtime;if(mode==5)throw error;}};
            Trace.action=new Runnable(){public void run(){Trace.actions++;require(Trace.events==(operation==0?712:7912)&&Trace.actualReceiver==actual&&Trace.captured==context&&Trace.observed==observed&&Trace.payload==sentinel);context.value=99;Trace.payload=mode==12?Integer.valueOf(8):mode==13?null:payload;if(mode==9){Trace.result=changed;Trace.source=replacement;}if(mode==6)throw runtime;if(mode==7)throw error;if(mode==8)UnusedScalarThrowsHarness.<RuntimeException>fail(checked);}};
            int result=-71;String text=null;Throwable failure=null;
            try{result=call(route,value);text=(String)Trace.payload;require(text.length()==9);}catch(Throwable caught){failure=caught;}
            final boolean early=mode==2,body=!early&&mode!=3&&mode!=4&&mode!=5,action=body&&mode!=1,done=action&&mode!=6&&mode!=7&&mode!=8;
            long events=early?0:7;if(body){if(operation==1)events=events*10+9;events=events*10+1;}if(action)events=events*10+2;if(done)events=events*10+3;
            require(Trace.events==events&&Trace.receivers==receivers+(early?0:1)&&Trace.preCalls==preCalls+(early?0:1)&&Trace.calls==calls+(body?1:0)&&Trace.overrides==overrides+(body&&operation==1?1:0)&&Trace.actions==actions+(action?1:0));
            require(Trace.prePayload==(early?sentinel:prePayload)&&Trace.payload==(action?(mode==12?Integer.valueOf(8):mode==13?null:payload):sentinel)&&Trace.actualReceiver==(body?actual:null)&&Trace.captured==(body?(Context)value:sentinelContext));
            require(Trace.observed==(action?observed:-31)&&context.value==(action?99:observed));
            int expectedResult=(mode==9||mode==15?changed:initial)^(operation==1?mask:0);
            require(result==(done?expectedResult:-71));
            Throwable expected=mode==4||mode==6?runtime:mode==5||mode==7?error:mode==8?checked:null;
            if(expected!=null)require(failure==expected);
            else if(mode==2||mode==12)require(failure instanceof ClassCastException);
            else if(mode==1||mode==3||mode==13)require(failure instanceof NullPointerException);
            else require(failure==null&&text==payload);
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
