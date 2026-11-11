import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ScopedCompletionCallbackHarness {
    static long events;
    static int failAt,contexts,calls,accepts;
    static Object token,seen,accepted,contextToken;
    static RuntimeException failure;
    static Completion expectedCompletion;
    static void require(boolean ok){if(!ok)throw new AssertionError("owned scoped completion callback");}
    static void hit(int n){events=events*10+n;if(failAt==n)throw failure;}
    static final class Invoke implements Callback {
        public Object apply(Object input,Object completion){calls++;require(input==Provider.scope && completion==expectedCompletion);seen=input;hit(2);((Completion)completion).accept(token);return token;}
    }
    static final class Finish implements Completion {
        public Object context(){contexts++;hit(1);return contextToken;}
        public void accept(Object value){accepts++;accepted=value;require(value==token);hit(3);}
    }
    static Object call(int route,Callback callback,Completion completion) throws Throwable {
        if(route==0)return Client.call(callback,completion);
        if(route==1)return ScopedCompletionCallbackCaller.call(callback,completion);
        if(route==2){try{return Provider.class.getMethod("work",Callback.class,Completion.class).invoke(null,callback,completion);}catch(InvocationTargetException p){throw p.getCause();}}
        return (Object)MethodHandles.publicLookup().findStatic(Provider.class,"work",MethodType.methodType(Object.class,Callback.class,Completion.class)).invokeExact(callback,completion);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        Method method=Provider.class.getDeclaredMethod("work",Callback.class,Completion.class);
        require(method.getTypeParameters().length==1 && method.getTypeParameters()[0].getBounds()[0]==Object.class && method.getGenericReturnType()==Object.class);
        ParameterizedType cb=(ParameterizedType)method.getGenericParameterTypes()[0],out=(ParameterizedType)method.getGenericParameterTypes()[1];
        require(cb.getRawType()==Callback.class && out.getRawType()==Completion.class);
        require(((WildcardType)cb.getActualTypeArguments()[0]).getLowerBounds()[0]==Scope.class);
        ParameterizedType nested=(ParameterizedType)((WildcardType)cb.getActualTypeArguments()[1]).getLowerBounds()[0];
        require(nested.getRawType()==Completion.class && ((WildcardType)nested.getActualTypeArguments()[0]).getLowerBounds()[0].equals(method.getTypeParameters()[0]));
        require(((WildcardType)cb.getActualTypeArguments()[2]).getUpperBounds()[0]==Object.class);
        require(((WildcardType)out.getActualTypeArguments()[0]).getLowerBounds()[0].equals(method.getTypeParameters()[0]));
        int[] failures={0,0,0,0,1,2,3,0,0,2,3,0,0,0};
        for(int attempt=0;attempt<2;attempt++){
            token=mode==1?null:mode==12?new Long(91+attempt):new Object();
            contextToken=new Object();failure=new RuntimeException("owned callback failure");
            events=0;contexts=calls=accepts=0;seen=accepted=null;failAt=failures[mode];
            Provider.context=Provider.seenResult=null;Provider.saved=null;Provider.savedCompletion=null;
            Provider.scope=mode==12?null:new Scope(mode==11?Long.MIN_VALUE:mode==13?Long.MAX_VALUE:0x1234567887654321L+attempt);Provider.savedScope=Provider.seenScope=null;
            Provider.sentinel=new Object();Provider.deferred=mode>=7 && mode<=10;
            Callback callback=mode==3||mode==8?null:new Invoke();Completion completion=mode==2?null:new Finish();
            expectedCompletion=completion;Object result=null;Throwable caught=null;
            try{result=call(route,callback,completion);}catch(Throwable p){caught=p;}
            boolean initialFail=mode==2||mode==3||mode==4||mode==5||mode==6;
            if(mode==2||mode==3)require(caught instanceof NullPointerException);
            else if(initialFail)require(caught==failure);else require(caught==null);
            if(Provider.deferred){
                require(result==Provider.sentinel && Provider.saved==callback && Provider.savedCompletion==completion && Provider.savedScope==Provider.scope && events==1 && calls==0 && accepts==0);
                require(Provider.context==contextToken && Provider.seenResult==null);
                try{result=Provider.resume();}catch(Throwable p){caught=p;}
                if(mode==8)require(caught instanceof NullPointerException);
                else if(mode==9||mode==10)require(caught==failure);else require(caught==null && result==token);
            }else if(!initialFail)require(result==token);
            long expected=mode==2?0:mode==3||mode==4||mode==8?1:mode==5||mode==9?12:123;
            require(events==expected && contexts==(mode==2?0:1) && calls==((mode==2||mode==3||mode==4||mode==8)?0:1));
            require(accepts==((mode==2||mode==3||mode==4||mode==5||mode==8||mode==9)?0:1));
            require(Provider.context==((mode==2||mode==4)?null:contextToken));
            require(Provider.seenResult==((mode==2||mode==3||mode==4||mode==5||mode==6||mode==8||mode==9||mode==10)?null:token));
            if(accepts!=0)require(accepted==token);
            require(Provider.seenScope==((mode==2||mode==4)?null:Provider.scope));
            require(Provider.scope==null || Provider.scope.value==(mode==11?Long.MIN_VALUE:mode==13?Long.MAX_VALUE:0x1234567887654321L+attempt));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+events);
        }
    }
}
