import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class BoundedFrameConsumerHarness {
    static long events;
    static int failAt,contexts,calls,accepts;
    static Object token,seen,accepted,contextToken;
    static RuntimeException failure;
    static void require(boolean ok){if(!ok)throw new AssertionError("owned bounded frame consumer");}
    static void hit(int n){events=events*10+n;if(failAt==n)throw failure;}
    static final class Invoke implements Callback {
        public Object apply(Object input){calls++;require(input instanceof Long && ((Long)input).longValue()==Provider.clock);seen=input;hit(2);return token;}
    }
    static final class Finish implements Completion {
        public Object context(){contexts++;hit(1);return contextToken;}
        public void accept(Object value){accepts++;accepted=value;require(value==token);hit(3);}
    }
    static Object call(int route,Carrier carrier,Callback callback,Completion completion) throws Throwable {
        if(route==0)return Client.call(carrier,callback,completion);
        if(route==1)return BoundedFrameConsumerCaller.call(carrier,callback,completion);
        if(route==2){try{return Provider.class.getMethod("access",Carrier.class,Callback.class,Completion.class).invoke(null,carrier,callback,completion);}catch(InvocationTargetException p){throw p.getCause();}}
        return (Object)MethodHandles.publicLookup().findStatic(Provider.class,"access",MethodType.methodType(Object.class,Carrier.class,Callback.class,Completion.class)).invokeExact(carrier,callback,completion);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        Method method=Provider.class.getDeclaredMethod("work",Carrier.class,Callback.class,Completion.class);
        require(method.getTypeParameters().length==3 && method.getTypeParameters()[0].getBounds()[0]==Object.class && method.getGenericReturnType()==Object.class);
        ParameterizedType cb=(ParameterizedType)method.getGenericParameterTypes()[1],out=(ParameterizedType)method.getGenericParameterTypes()[2];
        require(cb.getRawType()==Callback.class && out.getRawType()==Completion.class);
        require(((WildcardType)cb.getActualTypeArguments()[0]).getLowerBounds()[0]==Long.class);
        require(((WildcardType)cb.getActualTypeArguments()[1]).getUpperBounds()[0].equals(method.getTypeParameters()[0]));
        require(((WildcardType)out.getActualTypeArguments()[0]).getLowerBounds()[0].equals(method.getTypeParameters()[0]));
        int[] failures={0,0,0,0,1,2,3,0,0,2,3,0,0,0,0,7};
        for(int attempt=0;attempt<2;attempt++){
            token=mode==1?null:mode==12?new Long(91+attempt):new Object();
            contextToken=new Object();failure=new RuntimeException("owned callback failure");
            events=0;contexts=calls=accepts=0;seen=accepted=null;failAt=failures[mode];
            Provider.context=Provider.seenResult=null;Provider.saved=null;Provider.savedCompletion=null;Provider.seenCarrier=null;Provider.seenInfinite=false;
            Provider.clock=mode==11?Long.MIN_VALUE:mode==13?Long.MAX_VALUE:0x1234567887654321L+attempt;
            Provider.sentinel=new Object();Provider.deferred=mode>=7 && mode<=10;
            Callback callback=mode==3||mode==8?null:new Invoke();Completion completion=mode==2?null:new Finish();
            Carrier carrier=mode==14?null:new Carrier<Object,Bound>(){public boolean infinite(){hit(7);return mode==13;}};
            Object result=null;Throwable caught=null;
            try{result=call(route,carrier,callback,completion);}catch(Throwable p){caught=p;}
            boolean initialFail=mode==2||mode==3||mode==4||mode==5||mode==6||mode==14||mode==15;
            if(mode==2||mode==3||mode==14)require(caught instanceof NullPointerException);
            else if(initialFail)require(caught==failure);else require(caught==null);
            if(Provider.deferred){
                require(result==Provider.sentinel && Provider.saved==callback && Provider.savedCompletion==completion && events==71 && calls==0 && accepts==0);
                require(Provider.context==contextToken && Provider.seenResult==null);
                try{result=Provider.resume();}catch(Throwable p){caught=p;}
                if(mode==8)require(caught instanceof NullPointerException);
                else if(mode==9||mode==10)require(caught==failure);else require(caught==null && result==token);
            }else if(!initialFail)require(result==token);
            long expected=mode==14?0:mode==15?7:mode==2?7:mode==3||mode==4||mode==8?1:mode==5||mode==9?12:123;
            if(mode!=14&&mode!=15&&mode!=2)expected=Long.parseLong("7"+expected);
            require(events==expected && contexts==((mode==2||mode==14||mode==15)?0:1) && calls==((mode==2||mode==3||mode==4||mode==8||mode==14||mode==15)?0:1));
            require(accepts==((mode==2||mode==3||mode==4||mode==5||mode==8||mode==9||mode==14||mode==15)?0:1));
            require(Provider.context==((mode==2||mode==4||mode==14||mode==15)?null:contextToken));
            require(Provider.seenResult==((mode==2||mode==3||mode==4||mode==5||mode==8||mode==9||mode==14||mode==15)?null:token));
            if(accepts!=0)require(accepted==token);
            require(Provider.seenCarrier==carrier && Provider.seenInfinite==(mode==13));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+events);
        }
    }
}
