package fixture;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;
public final class InheritedFixedParameterBridgeHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned inherited fixed parameter bridge mismatch");}
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure)throws X{throw (X)failure;}
    private static final class Future extends Leaf {
        @Override public Result extra(String key){Trace.futures++;Trace.hit(9);return super.extra(key);}
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    private static Result call(int route,Object key)throws Throwable {
        if(route==0)return Core.typed((String)key);
        if(route==1)return Core.erased(key);
        if(route==2)return InheritedFixedParameterBridgeCaller.generic((Reader)Core.receiver(),key);
        if(route==3)return (Result)MethodHandles.privateLookupIn(Parent.class,MethodHandles.lookup()).findSpecial(
            Parent.class,"extra",MethodType.methodType(Result.class,Object.class),Parent.class).invokeExact((Parent)Core.receiver(),key);
        return (Result)MethodHandles.privateLookupIn(Leaf.class,MethodHandles.lookup()).findSpecial(
            Leaf.class,"extra",MethodType.methodType(Result.class,Object.class),Leaf.class).invokeExact(Core.receiver(),key);
    }
    private static void shape()throws Exception {
        for(Class<?> owner:new Class<?>[]{Parent.class,Leaf.class}) {
            Method bridge=owner.getDeclaredMethod("read",Object.class),typed=owner.getDeclaredMethod("read",String.class);
            require(bridge.isBridge()&&bridge.isSynthetic()&&bridge.getReturnType()==Result.class);
            require(!typed.isBridge()&&!typed.isSynthetic()&&typed.getReturnType()==Result.class&&typed.getTypeParameters().length==0);
            require(owner.getTypeParameters().length==0);
        }
        Method inheritedBridge=Parent.class.getDeclaredMethod("extra",Object.class),inheritedTyped=Parent.class.getDeclaredMethod("extra",String.class);
        require(inheritedBridge.isBridge()&&inheritedBridge.isSynthetic()&&!inheritedTyped.isBridge()&&!inheritedTyped.isSynthetic());
        require(Leaf.class.getMethod("extra",Object.class).getDeclaringClass()==Parent.class&&Leaf.class.getMethod("extra",String.class).getDeclaringClass()==Parent.class);
        for(Method declared:Leaf.class.getDeclaredMethods())require(!declared.getName().equals("extra"));
        require(Leaf.class.getGenericSuperclass()==Parent.class&&Leaf.class.getGenericInterfaces().length==0);
        ParameterizedType base=(ParameterizedType)Parent.class.getGenericSuperclass(),contract=(ParameterizedType)Parent.class.getGenericInterfaces()[0];
        require(base.getRawType()==Base.class&&base.getActualTypeArguments()[0]==String.class&&base.getActualTypeArguments()[1]==Object.class);
        require(contract.getRawType()==Reader.class&&contract.getActualTypeArguments()[0]==String.class);
    }
    private static void run(int route,int operation,final int mode,int attempt)throws Throwable {
        final Leaf receiver=operation==0?new Leaf():new Future(),replacement=operation==0?new Leaf():new Future(),actual=mode==12?replacement:receiver;
        final String key=new String(attempt==0?"owned-key":"other-key"),sentinelKey=new String("unread"),payload=new String("owned-payload");
        final Object value=mode==1?null:mode==2||mode==3?Integer.valueOf(17+attempt):key,sentinel=new Object(),prePayload=new Object(),badPayload=Integer.valueOf(23+attempt);
        final Result initial=mode==10?null:new Result(),changed=new Result(),sentinelResult=new Result();
        final RuntimeException runtime=new IllegalStateException("owned runtime");final Error error=new Error("owned error");final IOException checked=new IOException("owned checked");
        Trace.events=0;Trace.source=mode==3?null:receiver;Trace.result=initial;Trace.actualReceiver=null;Trace.key=sentinelKey;Trace.observed=-31;Trace.payload=Trace.prePayload=sentinel;
        final int receivers=Trace.receivers,calls=Trace.calls,leaves=Trace.leaves,futures=Trace.futures,preCalls=Trace.preCalls,actions=Trace.actions;
        final long prefix=operation==0?712:7912;
        Trace.before=new Runnable(){public void run(){Trace.preCalls++;require(Trace.events==7&&Trace.actualReceiver==null&&Trace.key==sentinelKey&&Trace.observed==-31&&Trace.payload==sentinel);Trace.prePayload=prePayload;if(mode==12)Trace.source=replacement;if(mode==4)throw runtime;if(mode==5)throw error;}};
        Trace.action=new Runnable(){public void run(){Trace.actions++;require(Trace.events==prefix&&Trace.actualReceiver==actual&&Trace.key==value&&Trace.observed==9&&Trace.payload==sentinel);Trace.payload=mode==14?badPayload:mode==15?null:payload;if(mode==9)Trace.result=changed;if(mode==11)Trace.result=null;if(mode==13)Trace.source=replacement;if(mode==6)throw runtime;if(mode==7)throw error;if(mode==8)InheritedFixedParameterBridgeHarness.<RuntimeException>fail(checked);}};
        Result result=sentinelResult;String text=null;Throwable failure=null;
        try{result=call(route,value);text=(String)Trace.payload;require(text.length()==13);}catch(Throwable caught){failure=caught;}
        boolean early=route==0&&(mode==2||mode==3),body=!early&&mode!=2&&mode!=3&&mode!=4&&mode!=5,action=body&&mode!=1,done=action&&mode!=6&&mode!=7&&mode!=8;
        long events=early?0:7;if(body){if(operation==1)events=events*10+9;events=events*10+1;}if(action)events=events*10+2;if(done)events=events*10+3;
        require(Trace.events==events&&Trace.receivers==receivers+(early?0:1)&&Trace.preCalls==preCalls+(early?0:1)&&Trace.calls==calls+(body?1:0)&&Trace.leaves==leaves&&Trace.futures==futures+(body&&operation==1?1:0)&&Trace.actions==actions+(action?1:0));
        require(Trace.prePayload==(early?sentinel:prePayload)&&Trace.payload==(action?(mode==14?badPayload:mode==15?null:payload):sentinel));
        require(Trace.actualReceiver==(body?actual:null)&&Trace.key==(body?value:sentinelKey)&&Trace.observed==(action?9:-31));
        require(Trace.source==(mode==3?null:mode==12||mode==13?replacement:receiver));
        require(result==(done?(mode==9?changed:mode==11?null:initial):sentinelResult));
        Throwable expected=mode==4||mode==6?runtime:mode==5||mode==7?error:mode==8?checked:null;
        if(expected!=null)require(failure==expected);
        else if(mode==2||mode==14||(mode==3&&route==0))require(failure instanceof ClassCastException);
        else if(mode==1||mode==3||mode==15)require(failure instanceof NullPointerException);
        else require(failure==null&&text==payload);
    }
    public static void main(String[] args)throws Throwable {
        shape();for(int attempt=0;attempt<2;attempt++)run(Integer.parseInt(args[0]),Integer.parseInt(args[1]),Integer.parseInt(args[2]),attempt);
        System.out.println("attempts=2,inherited-fixed-parameter-bridge=matched");
    }
}
