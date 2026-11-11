package fixture;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;
public final class AnonymousClosedParentBridgeHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned anonymous closed parent bridge mismatch");}
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure)throws X{throw (X)failure;}
    private static final class Future extends Parent {
        @Override public Value read(float fraction,Value left,Value right){Trace.futures++;Trace.hit(9);return super.read(fraction,left,right);}
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    private static Object call(int route,Parent receiver,float fraction,Object left,Object right)throws Throwable {
        if(route==0)return receiver.read(fraction,(Value)left,(Value)right);
        if(route==1)return ((Reader)receiver).read(fraction,left,right);
        if(route==2)return AnonymousClosedParentBridgeCaller.generic((Reader)receiver,fraction,left,right);
        if(route==3){
            try{return Reader.class.getMethod("read",float.class,Object.class,Object.class).invoke(receiver,Float.valueOf(fraction),left,right);}
            catch(InvocationTargetException failure){throw failure.getCause();}
        }
        return (Object)MethodHandles.lookup().findVirtual(Reader.class,"read",
            MethodType.methodType(Object.class,float.class,Object.class,Object.class)).invokeExact((Reader)receiver,fraction,left,right);
    }
    private static void shape(Parent receiver,Host host,boolean anonymous)throws Exception {
        Class<?> owner=receiver.getClass();
        Method bridge=owner.getDeclaredMethod("read",float.class,Object.class,Object.class);
        Method typed=owner.getDeclaredMethod("read",float.class,Value.class,Value.class);
        require(bridge.isBridge()&&bridge.isSynthetic()&&bridge.getReturnType()==Object.class&&bridge.getExceptionTypes().length==0);
        require(!typed.isBridge()&&!typed.isSynthetic()&&typed.getReturnType()==Value.class&&typed.getTypeParameters().length==0);
        require(owner.getTypeParameters().length==0);
        if(anonymous){
            require(owner.getGenericSuperclass()==Parent.class&&owner.getGenericInterfaces().length==0);
            require(owner.getEnclosingMethod().equals(Host.class.getDeclaredMethod("make")));
            java.lang.reflect.Field capture=owner.getDeclaredField("this$0");capture.setAccessible(true);require(capture.get(receiver)==host);
        }
        ParameterizedType contract=(ParameterizedType)Parent.class.getGenericInterfaces()[0];
        require(contract.getRawType()==Reader.class&&contract.getActualTypeArguments()[0]==Value.class);
    }
    private static void run(int route,int branch,final int mode,int attempt)throws Throwable {
        final Host host=new Host();final Parent instance=branch==1?host.make():attempt==0?new Parent():new Future();
        shape(instance,host,branch==1);
        final Parent receiver=mode==6?null:instance;
        final Value a=new Value(),b=new Value(),initial=new Value(),changed=new Value();a.code=31+attempt;b.code=47+attempt;
        final Object left=mode==1||mode==5?null:mode==3?new Object():a;
        final Object right=mode==2?null:mode==4||mode==5?new Object():mode==12?a:b;
        final int bits=mode==13?0x7fc12345:mode==14?0x80000000:mode==15?0xff800000:0x3e800000;
        final float fraction=Float.intBitsToFloat(bits);
        final Object sentinel=new Object(),payload=new Object();
        final RuntimeException runtime=new IllegalStateException("owned runtime");final Error error=new Error("owned error");final IOException checked=new IOException("owned checked");
        Trace.events=0;Trace.actualReceiver=null;Trace.fraction=Float.intBitsToFloat(0x3f400000);Trace.host=Trace.left=Trace.right=Trace.payload=sentinel;Trace.observedLeft=-31;Trace.observedRight=-47;
        Trace.result=mode==11?null:mode==12?a:initial;
        final int parents=Trace.parents,children=Trace.children,futures=Trace.futures,actions=Trace.actions,calls=host.calls;
        final long prefix=branch==1?1234:attempt==1?9234:234;
        Trace.action=new Runnable(){public void run(){Trace.actions++;require(Trace.events==prefix&&Trace.actualReceiver==instance&&Trace.left==left&&Trace.right==right&&Float.floatToRawIntBits(Trace.fraction)==bits&&Trace.observedLeft==a.code&&Trace.observedRight==((Value)right).code&&Trace.payload==sentinel);Trace.payload=payload;if(mode==10)Trace.result=changed;if(mode==7)throw runtime;if(mode==8)throw error;if(mode==9)AnonymousClosedParentBridgeHarness.<RuntimeException>fail(checked);}};
        Object result=sentinel;Throwable failure=null;
        try{result=call(route,receiver,fraction,left,right);}catch(Throwable caught){failure=caught;}
        final boolean body=mode!=3&&mode!=4&&mode!=5&&mode!=6,leftRead=body&&mode!=1,rightRead=leftRead&&mode!=2,completed=rightRead&&mode!=7&&mode!=8&&mode!=9;
        long events=body?(branch==1?1:attempt==1?9:0):0;if(body)events=events*10+2;if(leftRead)events=events*10+3;if(rightRead)events=events*10+4;if(completed)events=events*10+5;
        require(Trace.events==events&&Trace.parents==parents+(body?1:0)&&Trace.children==children+(body&&branch==1?1:0)&&Trace.futures==futures+(body&&branch==0&&attempt==1?1:0)&&Trace.actions==actions+(rightRead?1:0)&&host.calls==calls+(body&&branch==1?1:0));
        require(Trace.actualReceiver==(body?instance:null)&&Trace.host==(body&&branch==1?host:sentinel)&&Trace.left==(body?left:sentinel)&&Trace.right==(body?right:sentinel));
        require(Float.floatToRawIntBits(Trace.fraction)==(body?bits:0x3f400000)&&Trace.observedLeft==(leftRead?a.code:-31)&&Trace.observedRight==(rightRead?((Value)right).code:-47)&&Trace.payload==(rightRead?payload:sentinel));
        require(result==(completed?(mode==10?changed:mode==11?null:mode==12?a:initial):sentinel));
        if(mode==7||mode==8||mode==9)require(failure==(mode==7?runtime:mode==8?error:checked));
        else if(mode==3||mode==4||mode==5)require(failure instanceof ClassCastException);
        else if(mode==1||mode==2||mode==6)require(failure instanceof NullPointerException);
        else require(failure==null);
    }
    public static void main(String[] args)throws Throwable {
        for(int attempt=0;attempt<2;attempt++)run(Integer.parseInt(args[0]),Integer.parseInt(args[1]),Integer.parseInt(args[2]),attempt);
        System.out.println("attempts=2,anonymous-closed-parent-bridge=matched");
    }
}
