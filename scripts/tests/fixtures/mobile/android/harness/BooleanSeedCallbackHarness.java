import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class BooleanSeedCallbackHarness {
    static long events;static int failAt;static boolean flag;static RuntimeException failure;static Object a,b,result,accepted;static Bound vector;static Carrier carrier;
    static void require(boolean ok){if(!ok)throw new AssertionError("owned boolean seed callback");}
    static void hit(int n){events=events*10+n;if(failAt==n)throw failure;}
    static final class Read implements Carrier {public Object read(){hit(1);return a;}public Bound vector(){hit(2);return vector;}}
    static final class Seed implements Spec {public Object read(){hit(3);return b;}}
    static final class Invoke implements Action {public Object apply(Object c){require(c==carrier);hit(4);return result;}}
    static final class Finish implements Completion {public void accept(Object value){require(value==result);accepted=value;hit(5);}}
    static Object call(int route,Spec spec,Action action,Completion completion)throws Throwable{
        if(route==0)return Client.call(carrier,spec,flag,action,completion);
        if(route==1)return BooleanSeedCallbackCaller.call(carrier,spec,flag,action,completion);
        if(route==2){try{return Provider.class.getMethod("work",Carrier.class,Spec.class,boolean.class,Action.class,Completion.class).invoke(null,carrier,spec,flag,action,completion);}catch(InvocationTargetException f){throw f.getCause();}}
        return (Object)MethodHandles.publicLookup().findStatic(Provider.class,"work",MethodType.methodType(Object.class,Carrier.class,Spec.class,boolean.class,Action.class,Completion.class)).invokeExact(carrier,spec,flag,action,completion);
    }
    public static void main(String[] args)throws Throwable{
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);Method m=Provider.class.getDeclaredMethod("work",Carrier.class,Spec.class,boolean.class,Action.class,Completion.class);
        require(m.getTypeParameters().length==2 && m.getTypeParameters()[0].getBounds()[0]==Object.class && m.getTypeParameters()[1].getBounds()[0]==Bound.class);
        Type[] p=m.getGenericParameterTypes();require(p[2]==boolean.class && ((ParameterizedType)p[1]).getActualTypeArguments()[0].equals(m.getTypeParameters()[0]));
        ParameterizedType cb=(ParameterizedType)p[3],completionType=(ParameterizedType)p[4];require(((WildcardType)cb.getActualTypeArguments()[0]).getLowerBounds()[0].equals(p[0]) && cb.getActualTypeArguments()[1]==Unit.class && ((WildcardType)completionType.getActualTypeArguments()[0]).getLowerBounds()[0]==Unit.class);
        for(int attempt=0;attempt<2;attempt++){
            flag=attempt==0;a=mode==1?null:new Object();b=new Object();result=mode==12?new Object():mode==13?null:Unit.VALUE;vector=mode==14?null:new Bound(mode==15?Long.MIN_VALUE:0x1234567887654321L+attempt);
            events=0;failAt=mode>=6&&mode<=10?mode-5:0;failure=new RuntimeException("owned seed failure");accepted=null;
            Provider.seenCarrier=Provider.seenSpec=Provider.first=Provider.second=Provider.seenUnit=null;Provider.seenVector=null;Provider.seenFlag=!flag;
            carrier=mode==2?null:new Read();Spec spec=mode==3?null:new Seed();Action action=mode==4?null:new Invoke();Completion completion=mode==5?null:new Finish();
            Object actual=null;Throwable caught=null;try{actual=call(route,spec,action,completion);}catch(Throwable f){caught=f;}
            if(mode>=6&&mode<=10)require(caught==failure);else if(mode>=2&&mode<=5)require(caught instanceof NullPointerException);else if(mode==12)require(caught instanceof ClassCastException);else require(caught==null && actual==(flag?a:b));
            long expected=mode==2?0:mode==6?1:mode==3||mode==7?12:mode==4||mode==8?123:mode==5||mode==9||mode==12?1234:12345;require(events==expected);
            require(Provider.seenCarrier==carrier && Provider.seenSpec==spec && Provider.seenFlag==flag);
            require(Provider.first==((expected==0||mode==6)?null:a) && Provider.seenVector==((expected==0||expected==1||mode==7)?null:vector) && Provider.second==((expected==0||expected==1||expected==12||mode==8)?null:b));
            require(Provider.seenUnit==((expected<1234||mode==9||mode==12)?null:result));if(expected==12345)require(accepted==result);else require(accepted==null);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+expected);
        }
    }
}
